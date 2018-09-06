# -*- coding: utf-8 -*-

# FOGLAMP_BEGIN
# See: http://foglamp.readthedocs.io/
# FOGLAMP_END

from aiohttp import web
import urllib.parse
from foglamp.services.core import connect
from foglamp.common.configuration_manager import ConfigurationManager
from foglamp.common.storage_client.payload_builder import PayloadBuilder
from foglamp.common.audit_logger import AuditLogger

__author__ = "Amarendra K. Sinha, Ashish Jabble"
__copyright__ = "Copyright (c) 2017 OSIsoft, LLC"
__license__ = "Apache 2.0"
__version__ = "${VERSION}"

_help = """
    --------------------------------------------------------------------------------
    | GET POST       | /foglamp/category                                           |
    | GET            | /foglamp/category/{category_name}                           |
    | GET POST PUT   | /foglamp/category/{category_name}/{config_item}             |
    | DELETE         | /foglamp/category/{category_name}/{config_item}/value       |
    | GET POST       | /foglamp/category/{category_name}/children                  |
    | DELETE         | /foglamp/category/{category_name}/children/{child_category} |
    | DELETE         | /foglamp/category/{category_name}/parent                    |
    --------------------------------------------------------------------------------
"""

#################################
#  Configuration Manager
#################################


async def get_categories(request):
    """
    Args:
         request:

    Returns:
            the list of known categories in the configuration database

    :Example:
            curl -sX GET http://localhost:8081/foglamp/category
            curl -sX GET http://localhost:8081/foglamp/category?root=true
            curl -sX GET 'http://localhost:8081/foglamp/category?root=true&children=true'
    """
    cf_mgr = ConfigurationManager(connect.get_storage_async())

    if 'root' in request.query and request.query['root'].lower() in ['true', 'false']:
        is_root = True if request.query['root'].lower() == 'true' else False
        # to get nested categories, if children is true
        is_children = True if 'children' in request.query and request.query['children'].lower() == 'true' else False
        if is_children:
            categories_json = await cf_mgr.get_all_category_names(root=is_root, children=is_children)
        else:
            categories = await cf_mgr.get_all_category_names(root=is_root)
            categories_json = [{"key": c[0], "description": c[1]} for c in categories]
    else:
        categories = await cf_mgr.get_all_category_names()
        categories_json = [{"key": c[0], "description": c[1]} for c in categories]

    return web.json_response({'categories': categories_json})


async def get_category(request):
    """
    Args:
         request: category_name is required

    Returns:
            the configuration items in the given category.

    :Example:
            curl -X GET http://localhost:8081/foglamp/category/PURGE_READ
    """
    category_name = request.match_info.get('category_name', None)
    category_name = urllib.parse.unquote(category_name) if category_name is not None else None

    # TODO: make it optimized and elegant
    cf_mgr = ConfigurationManager(connect.get_storage_async())
    category = await cf_mgr.get_category_all_items(category_name)

    if category is None:
        raise web.HTTPNotFound(reason="No such Category found for {}".format(category_name))

    return web.json_response(category)


async def create_category(request):
    """
    Args:
         request: A JSON object that defines the category

    Returns:
            category info

    :Example:
            curl -d '{"key": "TEST", "description": "description", "value": {"info": {"description": "Test", "type": "boolean", "default": "true"}}}' -X POST http://localhost:8081/foglamp/category
            curl -d '{"key": "TEST", "description": "description", "value": {"info": {"description": "Test", "type": "boolean", "default": "true"}}, "children":["child1", "child2"]}' -X POST http://localhost:8081/foglamp/category
    """
    keep_original_items = None
    if 'keep_original_items' in request.query and request.query['keep_original_items'] != '':
        keep_original_items = request.query['keep_original_items'].lower()
        if keep_original_items not in ['true', 'false']:
            raise ValueError("Only 'true' and 'false' are allowed for keep_original_items. {} given.".format(keep_original_items))

    try:
        cf_mgr = ConfigurationManager(connect.get_storage_async())
        data = await request.json()
        if not isinstance(data, dict):
            raise ValueError('Data payload must be a dictionary')

        valid_post_keys = ['key', 'description', 'value']
        for k in valid_post_keys:
            if k not in list(data.keys()):
                raise KeyError("'{}' param required to create a category".format(k))

        category_name = data.get('key')
        category_desc = data.get('description')
        category_value = data.get('value')

        should_keep_original_items = True if keep_original_items == 'true' else False

        await cf_mgr.create_category(category_name=category_name, category_description=category_desc,
                                     category_value=category_value, keep_original_items=should_keep_original_items)

        category_info = await cf_mgr.get_category_all_items(category_name=category_name)
        if category_info is None:
            raise LookupError('No such %s found' % category_name)

        result = {"key": category_name, "description": category_desc, "value": category_info}
        if data.get('children'):
            r = await cf_mgr.create_child_category(category_name, data.get('children'))
            result.update(r)

    except (KeyError, ValueError, TypeError) as ex:
        raise web.HTTPBadRequest(reason=str(ex))

    except LookupError as ex:
        raise web.HTTPNotFound(reason=str(ex))

    except Exception as ex:
        raise web.HTTPException(reason=str(ex))

    return web.json_response(result)


async def get_category_item(request):
    """
    Args:
         request: category_name & config_item are required

    Returns:
            the configuration item in the given category.

    :Example:
            curl -X GET http://localhost:8081/foglamp/category/PURGE_READ/age
    """
    category_name = request.match_info.get('category_name', None)
    config_item = request.match_info.get('config_item', None)

    category_name = urllib.parse.unquote(category_name) if category_name is not None else None
    config_item = urllib.parse.unquote(config_item) if config_item is not None else None

    # TODO: make it optimized and elegant
    cf_mgr = ConfigurationManager(connect.get_storage_async())
    category_item = await cf_mgr.get_category_item(category_name, config_item)

    if category_item is None:
        raise web.HTTPNotFound(reason="No such Category item found for {}".format(config_item))

    return web.json_response(category_item)


async def set_configuration_item(request):
    """
    Args:
         request: category_name, config_item, {"value" : "<some value>"} are required

    Returns:
            set the configuration item value in the given category.

    :Example:
        curl -X PUT -H "Content-Type: application/json" -d '{"value": "<some value>" }' http://localhost:8081/foglamp/category/{category_name}/{config_item}

        For {category_name}=>PURGE update value for {config_item}=>age
        curl -X PUT -H "Content-Type: application/json" -d '{"value": "24"}' http://localhost:8081/foglamp/category/PURGE_READ/age

    """
    category_name = request.match_info.get('category_name', None)
    config_item = request.match_info.get('config_item', None)

    category_name = urllib.parse.unquote(category_name) if category_name is not None else None
    config_item = urllib.parse.unquote(config_item) if config_item is not None else None

    data = await request.json()
    cf_mgr = ConfigurationManager(connect.get_storage_async())

    try:
        value = data['value']
        if isinstance(value, dict):
            pass
        elif not isinstance(value, str):
            raise web.HTTPBadRequest(reason='{} should be a string literal, in double quotes'.format(value))
    except KeyError:
        raise web.HTTPBadRequest(reason='Missing required value for {}'.format(config_item))

    try:
        await cf_mgr.set_category_item_value_entry(category_name, config_item, value)
    except ValueError as ex:
        raise web.HTTPNotFound(reason=ex)
    except TypeError as ex:
        raise web.HTTPBadRequest(reason=ex)

    result = await cf_mgr.get_category_item(category_name, config_item)
    if result is None:
        raise web.HTTPNotFound(reason="No detail found for the category_name: {} and config_item: {}".format(category_name, config_item))

    return web.json_response(result)


async def delete_configuration_item_value(request):
    """
    Args:
        request: category_name, config_item are required

    Returns:
        set the configuration item value to empty string in the given category

    :Example:
        curl -X DELETE http://localhost:8081/foglamp/category/{category_name}/{config_item}/value

        For {category_name}=>PURGE delete value for {config_item}=>age
        curl -X DELETE http://localhost:8081/foglamp/category/PURGE_READ/age/value

    """
    category_name = request.match_info.get('category_name', None)
    config_item = request.match_info.get('config_item', None)

    category_name = urllib.parse.unquote(category_name) if category_name is not None else None
    config_item = urllib.parse.unquote(config_item) if config_item is not None else None

    # TODO: make it optimized and elegant
    cf_mgr = ConfigurationManager(connect.get_storage_async())
    try:
        category_item = await cf_mgr.get_category_item(category_name, config_item)
        if category_item is None:
            raise ValueError

        await cf_mgr.set_category_item_value_entry(category_name, config_item, category_item['default'])
    except ValueError:
        raise web.HTTPNotFound(reason="No detail found for the category_name: {} and config_item: {}".format(category_name, config_item))

    result = await cf_mgr.get_category_item(category_name, config_item)

    if result is None:
        raise web.HTTPNotFound(reason="No detail found for the category_name: {} and config_item: {}".format(category_name, config_item))

    return web.json_response(result)


async def get_child_category(request):
    """
    Args:
         request: category_name is required

    Returns:
            list of categories that are children of name category

    :Example:
            curl -X GET http://localhost:8081/foglamp/category/south/children
    """
    category_name = request.match_info.get('category_name', None)
    category_name = urllib.parse.unquote(category_name) if category_name is not None else None

    cf_mgr = ConfigurationManager(connect.get_storage_async())

    try:
        result = await cf_mgr.get_category_child(category_name)
    except ValueError as ex:
        raise web.HTTPNotFound(reason=str(ex))

    return web.json_response({"categories": result})


async def create_child_category(request):
    """
    Args:
         request: category_name is required and JSON object that defines the child category

    Returns:
        parent of the children being added

    :Example:
            curl -d '{"children": ["coap", "http", "sinusoid"]}' -X POST http://localhost:8081/foglamp/category/south/children
    """
    cf_mgr = ConfigurationManager(connect.get_storage_async())
    data = await request.json()
    if not isinstance(data, dict):
        raise ValueError('Data payload must be a dictionary')

    category_name = request.match_info.get('category_name', None)
    category_name = urllib.parse.unquote(category_name) if category_name is not None else None

    children = data.get('children')

    try:
        r = await cf_mgr.create_child_category(category_name, children)
    except TypeError as ex:
        raise web.HTTPBadRequest(reason=str(ex))
    except ValueError as ex:
        raise web.HTTPNotFound(reason=str(ex))

    return web.json_response(r)


async def delete_child_category(request):
    """
    Args:
        request: category_name, child_category are required

    Returns:
        remove the link b/w child category and its parent

    :Example:
        curl -X DELETE http://localhost:8081/foglamp/category/{category_name}/children/{child_category}

    """
    category_name = request.match_info.get('category_name', None)
    child_category = request.match_info.get('child_category', None)
    category_name = urllib.parse.unquote(category_name) if category_name is not None else None

    cf_mgr = ConfigurationManager(connect.get_storage_async())
    try:
        result = await cf_mgr.delete_child_category(category_name, child_category)

    except TypeError as ex:
        raise web.HTTPBadRequest(reason=str(ex))
    except ValueError as ex:
        raise web.HTTPNotFound(reason=str(ex))

    return web.json_response({"children": result})


async def delete_parent_category(request):
    """
    Args:
        request: category_name

    Returns:
        remove the link b/w parent-child category for the parent

    :Example:
        curl -X DELETE http://localhost:8081/foglamp/category/{category_name}/parent

    """
    category_name = request.match_info.get('category_name', None)
    category_name = urllib.parse.unquote(category_name) if category_name is not None else None

    cf_mgr = ConfigurationManager(connect.get_storage_async())
    try:
        await cf_mgr.delete_parent_category(category_name)
    except TypeError as ex:
        raise web.HTTPBadRequest(reason=str(ex))
    except ValueError as ex:
        raise web.HTTPNotFound(reason=str(ex))

    return web.json_response({"message": "Parent-child relationship for the parent-{} is deleted".format(category_name)})

/*
 * FogLAMP readings ingest.
 *
 * Copyright (c) 2018 OSisoft, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Mark Riddoch, Massimiliano Pinto, Amandeep Singh Arora
 */
#include <ingest.h>
#include <reading.h>
#include <config_handler.h>
#include <thread>
#include <logger.h>

using namespace std;

/**
 * Thread to process the ingest queue and send the data
 * to the storage layer.
 */
static void ingestThread(Ingest *ingest)
{
	while (ingest->running())
	{
		ingest->waitForQueue();
		ingest->processQueue();
	}
}

/**
 * Fetch all asset tracking tuples from DB and populate local cache
 *
 * @param m_mgtClient	Management client handle
 */
void Ingest::populateAssetTrackingCache(ManagementClient *mgtClient)
{
	try {
		std::vector<AssetTrackingTuple*>& vec = mgtClient->getAssetTrackingTuples(m_serviceName);
		for (AssetTrackingTuple* & rec : vec)
			{
			if (rec->m_pluginName != m_pluginName || rec->m_eventName != "Ingest")
				{
				m_logger->info("Plugin/event name mismatch; NOT adding asset tracker tuple to cache: '%s'", rec->assetToString().c_str());
				delete rec;
				continue;
				}
			assetTrackerTuplesCache.insert(rec);
			//m_logger->info("Added asset tracker tuple to cache: '%s'", rec->assetToString().c_str());
			}
		delete (&vec);
		}
	catch (...)
		{
		m_logger->error("Failed to populate asset tracking tuples' cache");
		return;
		}
}

/**
 * Check local cache for a given asset tracking tuple
 *
 * @param tuple		Tuple to find in cache
 * @return			Returns whether tuple is present in cache
 */
bool Ingest::checkAssetTrackingCache(AssetTrackingTuple& tuple)
{
	AssetTrackingTuple *ptr = &tuple;
	std::unordered_set<AssetTrackingTuple*>::const_iterator it = assetTrackerTuplesCache.find(ptr);
	if (it == assetTrackerTuplesCache.end())
		{
		return false;
		}
	else
		return true;
}

/**
 * Add asset tracking tuple via microservice management API and in cache
 *
 * @param tuple		New tuple to add in DB and in cache
 */
void Ingest::addAssetTrackingTuple(AssetTrackingTuple& tuple)
{
	std::unordered_set<AssetTrackingTuple*>::const_iterator it = assetTrackerTuplesCache.find(&tuple);
	if (it == assetTrackerTuplesCache.end())
		{
		bool rv = m_mgtClient->addAssetTrackingTuple(tuple.m_serviceName, tuple.m_pluginName, tuple.m_assetName, "Ingest");
		if (rv) // insert into cache only if DB operation succeeded
			{
			AssetTrackingTuple *ptr = new AssetTrackingTuple(tuple);
			assetTrackerTuplesCache.insert(ptr);
			}
		}
	else
		m_logger->info("addAssetTrackingTuple(): Tuple already found in cache: '%s', not adding again", tuple.assetToString().c_str());
}

/**
 * Create a row for given assetName in statistics DB table, if not present already
 * The key checked/created in the table is "INGEST_<assetName>"
 * 
 * @param assetName     Asset name for the plugin that is sending readings
 */
int Ingest::createStatsDbEntry(const string& assetName)
{
	// Prepare foglamp.statistics update
	string statistics_key = "INGEST_" + assetName;
	for (auto & c: statistics_key) c = toupper(c);
	
	// SELECT * FROM foglamp.configuration WHERE key = categoryName
	const Condition conditionKey(Equals);
	Where *wKey = new Where("key", conditionKey, statistics_key);
	Query qKey(wKey);

	ResultSet* result = 0;
	try
	{
		// Query via storage client
		result = m_storage.queryTable("statistics", qKey);

		if (!result->rowCount())
		{
			// Prepare insert values for insertTable
			InsertValues newStatsEntry;
			newStatsEntry.push_back(InsertValue("key", statistics_key));
			newStatsEntry.push_back(InsertValue("description", string("Readings received from asset ")+assetName));
			// Set "value" field for insert using the JSON document object
			newStatsEntry.push_back(InsertValue("value", 0));
			newStatsEntry.push_back(InsertValue("previous_value", 0));

			// Do the insert
			if (!m_storage.insertTable("statistics", newStatsEntry))
			{
				m_logger->error("%s:%d : Insert new row into statistics table failed, newStatsEntry='%s'", __FUNCTION__, __LINE__, newStatsEntry.toJSON().c_str());
				return -1;
			}
		}
	}
	catch (...)
	{
		m_logger->error("%s:%d : Unable to create new row in statistics table with key='%s'", __FUNCTION__, __LINE__, statistics_key.c_str());
		return -1;
	}
	return 0;
}

/**
 * Thread to update statistics table in DB
 */
static void statsThread(Ingest *ingest)
{
	while (ingest->running())
	{
		ingest->updateStats();
	}
}

 /**
 * Update statistics for this south service. Successfully processed 
 * readings are reflected against plugin asset name and READINGS keys.
 * Discarded readings stats are updated against DISCARDED key.
 */
void Ingest::updateStats()
{
	unique_lock<mutex> lck(m_statsMutex);
	if (m_running) // don't wait on condition variable if plugin/ingest is being shutdown
		m_statsCv.wait(lck);

	if (statsPendingEntries.empty())
		{
		//Logger::getLogger()->info("statsPendingEntries is empty, returning from updateStats()");
		return;
		}
	
	int readings=0;
	vector<pair<ExpressionValues *, Where *>> statsUpdates;
	string key;
	const Condition conditionStat(Equals);
	
	for (auto it = statsPendingEntries.begin(); it != statsPendingEntries.end(); ++it)
		{
		if (statsDbEntriesCache.find(it->first) == statsDbEntriesCache.end())
			{
			createStatsDbEntry(it->first);
			statsDbEntriesCache.insert(it->first);
			//Logger::getLogger()->info("%s:%d : Created stats entry for asset name %s and added to cache", __FUNCTION__, __LINE__, it->first.c_str());
			}
		
		if (it->second)
			{
			// Prepare foglamp.statistics update
			key = "INGEST_" + it->first;
			for (auto & c: key) c = toupper(c);

			// Prepare "WHERE key = name
			Where *wPluginStat = new Where("key", conditionStat, key);

			// Prepare value = value + inc
			ExpressionValues *updateValue = new ExpressionValues;
			updateValue->push_back(Expression("value", "+", (int) it->second));

			statsUpdates.emplace_back(updateValue, wPluginStat);
			readings += it->second;
			}
		}

	if(readings)
		{
		Where *wPluginStat = new Where("key", conditionStat, "READINGS");
		ExpressionValues *updateValue = new ExpressionValues;
		updateValue->push_back(Expression("value", "+", (int) readings));
		statsUpdates.emplace_back(updateValue, wPluginStat);
		}
	if (m_discardedReadings)
		{
		Where *wPluginStat = new Where("key", conditionStat, "DISCARDED");
		ExpressionValues *updateValue = new ExpressionValues;
		updateValue->push_back(Expression("value", "+", (int) m_discardedReadings));
		statsUpdates.emplace_back(updateValue, wPluginStat);
 		}
	
	try
		{
		int rv = m_storage.updateTable("statistics", statsUpdates);
		
		if (rv<0)
			Logger::getLogger()->info("%s:%d : Update stats failed, rv=%d", __FUNCTION__, __LINE__, rv);
		else
			{
			m_discardedReadings=0;
			for (auto it = statsUpdates.begin(); it != statsUpdates.end(); ++it)
				{
				delete it->first;
				delete it->second;
				}
			statsPendingEntries.clear();
			}
		}
	catch (...)
		{
		Logger::getLogger()->info("%s:%d : Statistics table update failed, will retry on next iteration", __FUNCTION__, __LINE__);
		}
}

/**
 * Construct an Ingest class to handle the readings queue.
 * A seperate thread is used to send the readings to the
 * storage layer based on time. This thread in created in
 * the constructor and will terminate when the destructor
 * is called.
 * TODO - try to reduce the number of arguments in c'tor
 *
 * @param storage	The storage client to use
 * @param timeout	Maximum time before sending a queue of readings in milliseconds
 * @param threshold	Length of queue before sending readings
 */
Ingest::Ingest(StorageClient& storage,
		unsigned long timeout,
		unsigned int threshold,
		const std::string& serviceName,
		const std::string& pluginName,
		ManagementClient *mgmtClient) :
			m_storage(storage),
			m_timeout(timeout),
			m_queueSizeThreshold(threshold),
			m_serviceName(serviceName),
			m_pluginName(pluginName),
			m_mgtClient(mgmtClient)
{
	m_running = true;
	m_queue = new vector<Reading *>();
	m_thread = new thread(ingestThread, this);
	m_statsThread = new thread(statsThread, this);
	m_logger = Logger::getLogger();
	m_data = NULL;
	m_discardedReadings = 0;
	
	// populate asset tracking cache
	populateAssetTrackingCache(m_mgtClient);
}

/**
 * Destructor for the Ingest class
 *
 * Set's the running flag to false. This will
 * cause the processing thread to drain the queue
 * and then exit.
 * Once this thread has exited the destructor will
 * return.
 */
Ingest::~Ingest()
{
	m_running = false;
	m_cv.notify_one();
	m_thread->join();
	processQueue();
	m_statsThread->join();
	updateStats();
	delete m_queue;
	delete m_thread;
	delete m_statsThread;
	//delete m_data;
	
	// Cleanup filters
	FilterPlugin::cleanupFilters(m_filters, m_serviceName);
}

/**
 * Check if the ingest process is still running.
 * This becomes false when the service is shutdown
 * and is used to allow the queue to drain and then
 * the procssing routine to terminate.
 */
bool Ingest::running()
{
	return m_running;
}

/**
 * Add a reading to the reading queue
 */
void Ingest::ingest(const Reading& reading)
{
	lock_guard<mutex> guard(m_qMutex);
	m_queue->push_back(new Reading(reading));
	if (m_queue->size() >= m_queueSizeThreshold || m_running == false)
		m_cv.notify_all();
}

/**
 * Add a reading to the reading queue
 */
void Ingest::ingest(const vector<Reading *> *vec)
{
	lock_guard<mutex> guard(m_qMutex);
	
	// Get the readings in the set
	for (auto & rdng : *vec)
	{
		m_queue->push_back(rdng);
	}
	if (m_queue->size() >= m_queueSizeThreshold || m_running == false)
		m_cv.notify_all();
}


void Ingest::waitForQueue()
{
	mutex mtx;
	unique_lock<mutex> lck(mtx);
	if (m_running)
		m_cv.wait_for(lck,chrono::milliseconds(m_timeout));
}

/**
 * Process the queue of readings.
 *
 * Send them to the storage layer as a block. If the append call
 * fails requeue the readings for the next transmission.
 *
 * In order not to lock the queue for an excessie time a new queue
 * is created and the old one moved to a local variable. This minimise
 * the time we hold the queue mutex to the time it takes to swap two
 * variables.
 */
void Ingest::processQueue()
{
bool requeue = false;
vector<Reading *>* newQ = new vector<Reading *>();

	// Block of code to execute holding the mutex
	{
		lock_guard<mutex> guard(m_qMutex);
		m_data = m_queue;
		m_queue = newQ;
	}
	
	/*
	 * Create a ReadingSet from m_data readings if we have filters.
	 *
	 * At this point the m_data vectoir is cleared so that the only reference to
	 * the readings is inthe ReadingSet that is passed along the filter pipeline
	 *
	 * The final filter in the pipeline will pass the ReadingSet back into the
	 * ingest class where it will repopulate the m_data member.
	 */
	if (m_filters.size())
	{
		auto it = m_filters.begin();
		ReadingSet *readingSet = new ReadingSet(m_data);
		m_data->clear();
		// Pass readingSet to filter chain
		(*it)->ingest(readingSet);

		/*
		 * If filtering removed all the readings then simply clean up m_data and
		 * return.
		 */
		if (m_data->size() == 0)
		{
			delete m_data;
			m_data = NULL;
			return;
		}
	}

	std::map<std::string, int>		statsEntriesCurrQueue;
	// check if this requires addition of a new asset tracker tuple
	for (vector<Reading *>::iterator it = m_data->begin(); it != m_data->end(); ++it)
	{
		Reading *reading = *it;
		AssetTrackingTuple tuple(m_serviceName, m_pluginName, reading->getAssetName(), "Ingest");
		if (!checkAssetTrackingCache(tuple))
			{
			addAssetTrackingTuple(tuple);
			m_logger->info("processQueue(): Added new asset tracking tuple seen during readings' ingest: %s", tuple.assetToString().c_str());
			}
		++statsEntriesCurrQueue[reading->getAssetName()];
	}
		
	/**
	 * 'm_data' vector is ready to be sent to storage service.
	 *
	 * Note: m_data might contain:
	 * - Readings set by the configured service "plugin" 
	 * OR
	 * - filtered readings by filter plugins in 'readingSet' object:
	 *	1- values only
	 *	2- some readings removed
	 *	3- New set of readings
	 */
	int rv = 0;
	if ((!m_data->empty()) &&
			(rv = m_storage.readingAppend(*m_data)) == false && requeue == true)
	{
		m_logger->error("Failed to write readings to storage layer, buffering");
		lock_guard<mutex> guard(m_qMutex);

		// BUffer current data in m_data
		m_queue->insert(m_queue->cbegin(),
				m_data->begin(),
				m_data->end());
		// Is it possible that some of the readings are stored in DB, and others are not?
	}
	else
	{	
		if (!m_data->empty() && rv==false) // m_data had some (possibly filtered) readings, but they couldn't be sent successfully to storage service
			{
			m_logger->info("%s:%d, Couldn't send %d readings to storage service", __FUNCTION__, __LINE__, m_data->size());
			m_discardedReadings += m_data->size();
			}
		else
			{
			unique_lock<mutex> lck(m_statsMutex);
			for (auto &it : statsEntriesCurrQueue)
				statsPendingEntries[it.first] += it.second;
			}
		
		// Remove the Readings in the vector
		for (vector<Reading *>::iterator it = m_data->begin();
						 it != m_data->end(); ++it)
		{
			Reading *reading = *it;
			delete reading;
		}
	}

	delete m_data;
	m_data = NULL;
	
	// Signal stats thread to update stats
	lock_guard<mutex> guard(m_statsMutex);
	m_statsCv.notify_all();
}

/**
 * Load filter plugins
 *
 * Filters found in configuration are loaded
 * and adde to the Ingest class instance
 *
 * @param categoryName	Configuration category name
 * @param ingest	The Ingest class reference
 *			Filters are added to m_filters member
 *			False for errors.
 * @return		True if filters were loaded and initialised
 *			or there are no filters
 *			False with load/init errors
 */
bool Ingest::loadFilters(const string& categoryName)
{
	// Try to load filters:
	if (!FilterPlugin::loadFilters(categoryName,
				       m_filters,
				       m_mgtClient))
	{
		// Return false on any error
		return false;
	}

	// Set up the filter pipeline
	return setupFiltersPipeline();
}

/**
 * Set the filterPipeline in the Ingest class
 * 
 * This method calls the the method "plugin_init" for all loadade filters.
 * Up to date filter configurations and Ingest filtering methods
 * are passed to "plugin_init"
 *
 * @param ingest	The ingest class
 * @return 		True on success,
 *			False otherwise.
 * @thown		Any caught exception
 */
bool Ingest::setupFiltersPipeline()
{
ConfigHandler	*configHandler = ConfigHandler::getInstance(m_mgtClient);
bool 		initErrors = false;
string		errMsg = "'plugin_init' failed for filter '";

	for (auto it = m_filters.begin(); it != m_filters.end(); ++it)
	{
		string filterCategoryName = (*it)->getName();
		ConfigCategory updatedCfg;
		vector<string> children;
        
		try
		{
			// Fetch up to date filter configuration
			updatedCfg = m_mgtClient->getCategory(filterCategoryName);

			// Add filter category name under service/process config name
			children.push_back(filterCategoryName);
			m_mgtClient->addChildCategories(m_serviceName, children);

        		configHandler->registerCategory(this, filterCategoryName);
			m_filterCategories.insert(pair<string, FilterPlugin *>(filterCategoryName, *it));
		}
		// TODO catch specific exceptions
		catch (...)
		{       
			throw;      
		}                   

		// Iterate the load filters set in the Ingest class m_filters member 
		if ((it + 1) != m_filters.end())
		{
			// Set next filter pointer as OUTPUT_HANDLE
			if (!(*it)->init(updatedCfg,
				    (OUTPUT_HANDLE *)(*(it + 1)),
				    Ingest::passToOnwardFilter))
			{
				errMsg += (*it)->getName() + "'";
				initErrors = true;
				break;
			}
		}
		else
		{
			// Set the Ingest class pointer as OUTPUT_HANDLE
			if (!(*it)->init(updatedCfg,
					 (OUTPUT_HANDLE *)this,
					 Ingest::useFilteredData))
			{
				errMsg += (*it)->getName() + "'";
				initErrors = true;
				break;
			}
		}

		if ((*it)->persistData())
		{
			// Plugin support SP_PERSIST_DATA
			// Instantiate the PluginData class
			(*it)->m_plugin_data = new PluginData(&m_storage);
			// Load plugin data from storage layer
			string pluginStoredData = (*it)->m_plugin_data->loadStoredData(m_serviceName + (*it)->getName());
 			//call 'plugin_start' with plugin data: startData()
			(*it)->startData(pluginStoredData);
		}
		else
		{
			// We don't call simple plugin_start for filters right now
		}
	}

	if (initErrors)
	{
		// Failure
		m_logger->fatal("%s error: %s", SERVICE_NAME, errMsg.c_str());
		return false;
	}

	//Success
	return true;
}

/**
 * Pass the current readings set to the next filter in the pipeline
 *
 * Note:
 * This routine must be passed to all filters "plugin_init" except the last one
 *
 * Static method
 *
 * @param outHandle     Pointer to next filter
 * @param readings      Current readings set
 */
void Ingest::passToOnwardFilter(OUTPUT_HANDLE *outHandle,
				READINGSET *readingSet)
{
	// Get next filter in the pipeline
	FilterPlugin *next = (FilterPlugin *)outHandle;
	// Pass readings to next filter
	next->ingest(readingSet);
}

/**
 * Use the current input readings (they have been filtered
 * by all filters)
 *
 * The assumption is that one of two things has happened.
 *
 *	1. The filtering has all been done in place. In which case
 *	the m_data vector is in the ReadingSet passed in here.
 *
 *	2. The fitlering has created new ReadingSet in which case
 *	the reading vector must be copied into m_data from the
 *	ReadingSet.
 *
 * Note:
 * This routine must be passed to last filter "plugin_init" only
 *
 * Static method
 *
 * @param outHandle     Pointer to Ingest class instance
 * @param readingSet    Filtered reading set being added to Ingest::m_data
 */
void Ingest::useFilteredData(OUTPUT_HANDLE *outHandle,
			     READINGSET *readingSet)
{
	Ingest* ingest = (Ingest *)outHandle;
	if (ingest->m_data != readingSet->getAllReadingsPtr())
	{
		ingest->m_data->clear();// Remove any pointers still in the vector
		*(ingest->m_data) = readingSet->getAllReadings();
	}
	readingSet->clear();
	delete readingSet;
}

/**
 * Configuration change for one of our filters. Lookup the category name and
 * find the plugin to call. Call the reconfigure method of that plugin with
 * the new configuration.
 *
 * Note when the filter pipeline is abstracted this will move to the pipeline
 *
 * @param category	The name of the configuration category
 * @param newConfig	The new category contents
 */
void Ingest::configChange(const string& category, const string& newConfig)
{
	auto it = m_filterCategories.find(category);
	if (it != m_filterCategories.end())
	{
		it->second->reconfigure(newConfig);
	}
}

.. |br| raw:: html

   <br />

**************************************
Building and using FogLAMP on Raspbian
**************************************

FogLAMP requires the use of Python 3.5 in order to support the
asynchronous IO mechanisms used by FogLAMP. Earlier Raspberry Pi Raspbian
distributions support Python 3.4 as the latest version of Python.
In order to build and run FogLAMP on Raspbian the version of Python
must be updated manually if your distribution has an older version.

**NOTE**: These steps must be executed *in addition* to what is described in the README file when you install FogLAMP on Raspbian.

Check your Python version by running the command
::
    python3 --version
|br|

Install the latest version of Python 3 on Raspbian stretch:
::
    sudo apt-get install python3
|br|

Confirm the Python version
::
    python3 --version
    pip3 --version
|br|

These should both return a version number as 3.5, if not then check which
python3 and pip3 you are running and replace these with new
versions.

Once python3.5 or greater has been installed you may follow the instructions
in the README file to build, install and run FogLAMP on Raspberry
Pi using the Raspbian distribution.

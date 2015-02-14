
/****************************************************************************************************

RepRapFirmware - Main Program

This firmware is intended to be a fully object-oriented highly modular control program for
RepRap self-replicating 3D printers.

It owes a lot to Marlin and to the original RepRap FiveD_GCode.


General design principles:

  * Control by RepRap G Codes.  These are taken to be machine independent, though some may be unsupported.
  * Full use of C++ OO techniques,
  * Make classes hide their data,
  * Make everything except the Platform class (see below) as stateless as possible,
  * No use of conditional compilation except for #include guards - if you need that, you should be
       forking the repository to make a new branch - let the repository take the strain,
  * Concentration of all machine-dependent defintions and code in Platform.h and Platform.cpp,
  * No specials for (X,Y) or (Z) - all movement is 3-dimensional,
  * Except in Platform.h, use real units (mm, seconds etc) throughout the rest of the code wherever possible,
  * Try to be efficient in memory use, but this is not critical,
  * Labour hard to be efficient in time use, and this is  critical,
  * Don't abhor floats - they work fast enough if you're clever,
  * Don't avoid arrays and structs/classes,
  * Don't avoid pointers,
  * Use operator and function overloading where appropriate.


Naming conventions:

  * #defines are all CAPITALS_WITH_OPTIONAL_UNDERSCORES_BETWEEN_WORDS
  * No underscores in other names - MakeReadableWithCapitalisation
  * Class names and functions start with a CapitalLetter
  * Variables start with a lowerCaseLetter
  * Use veryLongDescriptiveNames


Structure:

There are seven main classes:

  * RepRap
  * GCodes
  * Heat
  * Move
  * Platform
  * Network, and
  * Webserver

RepRap:

This is just a container class for the single instances of all the others, and otherwise does very little.

GCodes:

This class is fed GCodes, either from the web interface, or from GCode files, or from a serial interface,
Interprets them, and requests actions from the RepRap machine via the other classes.

Heat:

This class implements all heating and temperature control in the RepRap machine.

Move:

This class controls all movement of the RepRap machine, both along its axes, and in its extruder drives.

Platform:

This is the only class that knows anything about the physical setup of the RepRap machine and its
controlling electronics.  It implements the interface between all the other classes and the RepRap machine.
All the other classes are completely machine-independent (though they may declare arrays dimensioned
to values #defined in Platform.h).

Network:

This class implements a basic TCP interface for the Webserver classes using lwip.

Webserver:

This class talks to the network (via Platform) and implements a simple webserver to give an interactive
interface to the RepRap machine.  It uses the Knockout and Jquery Javascript libraries to achieve this.
In addition, FTP and Telnet servers are provided for easier SD card file management and G-Code handling.



When the software is running there is one single instance of each main class, and all the memory allocation is
done on initialization.  new/malloc should not be used in the general running code, and delete is never
used.  Each class has an Init() function that resets it to its boot-up state; the constructors merely handle
that memory allocation on startup.  Calling RepRap.Init() calls all the other Init()s in the right sequence.

There are other ancillary classes that are declared in the .h files for the master classes that use them.  For
example, Move has a DDA class that implements a Bresenham/digital differential analyser.


Timing:

There is a single interrupt chain entered via Platform.Interrupt().  This controls movement step timing, and
this chain of code should be the only place that volatile declarations and structure/variable-locking are
required.  All the rest of the code is called sequentially and repeatedly as follows:

All the main classes have a Spin() function.  These are called in a loop by the RepRap.Spin() function and implement
simple timesharing.  No class does, or ever should, wait inside one of its functions for anything to happen or call
any sort of delay() function.  The general rule is:

  Can I do a thing?
    Yes - do it
    No - set a flag/timer to remind me to do it next-time-I'm-called/at-a-future-time and return.

The restriction this strategy places on almost all the code in the firmware (that it must execute quickly and
never cause waits or delays) is balanced by the fact that none of that code needs to worry about synchronization,
locking, or other areas of code accessing items upon which it is working.  As mentioned, only the interrupt
chain needs to concern itself with such problems.  Unlike movement, heating (including PID controllers) does
not need the fast precision of timing that interrupts alone can offer.  Indeed, most heating code only needs
to execute a couple of times a second.

Most data is transferred bytewise, with classes' Spin() functions typically containing code like this:

  Is a byte available for me?
    Yes
      read it and add it to my buffer
      Is my buffer complete?
         Yes
           Act on the contents of my buffer
         No
           Return
  No
    Return

Note that it is simple to raise the "priority" of any class's activities relative to the others by calling its
Spin() function more than once from RepRap.Spin().

-----------------------------------------------------------------------------------------------------

Version 0.1

18 November 2012

Adrian Bowyer
RepRap Professional Ltd
http://reprappro.com

Licence: GPL

****************************************************************************************************/

// If this goes in the right place (Platform.h) the compile fails. Why? - AB

//#include <SPI.h>
//#include <Ethernet.h>
//#include <SD.h>

#include "RepRapFirmware.h"

// We just need one instance of RepRap; everything else is contained within it and hidden

RepRap reprap;

const char *moduleName[] =
{
	"Platform",
//	"Network",
//	"Webserver",
	"GCodes",
	"Move",
	"Heat",
	"DDA",
	"?","?","?","?","?","?","?","?",
	"none"
};

//*************************************************************************************************

// RepRap member functions.

// Do nothing more in the constructor; put what you want in RepRap:Init()

RepRap::RepRap() : active(false), debug(0), stopped(false), spinningModule(noModule), ticksInSpinState(0),
		resetting(false), fileInfoDetected(false), printStartTime(0.0), gcodeReply(gcodeReplyBuffer, GCODE_REPLY_LENGTH),
		currentLayer(0), firstLayerDuration(0.0), firstLayerHeight(0.0), firstLayerFilament(0.0), firstLayerProgress(0.0),
		warmUpDuration(0.0), layerEstimatedTimeLeft(0.0), lastLayerTime(0.0), lastLayerFilament(0.0), numLayerSamples(0)
{
  platform = new Platform();
//  network = new Network(platform);
//  webserver = new Webserver(platform, network);
//  gCodes = new GCodes(platform, webserver);
  gCodes = new GCodes(platform);
  move = new Move(platform, gCodes);
  heat = new Heat(platform, gCodes);
  toolList = NULL;
}

void RepRap::Init()
{
  debug = 0xffff;
  activeExtruders = 1;		// we always report at least 1 extruder to the web interface
  activeHeaters = 2;		// we always report the bed heater + 1 extruder heater to the web interface
  SetPassword(DEFAULT_PASSWORD);
  SetName(DEFAULT_NAME);

  beepFrequency = beepDuration = 0;
  message[0] = 0;

  gcodeReply[0] = 0;
  replySeq = 0;
  processingConfig = true;

  // All of the following init functions must execute reasonably quickly before the watchdog times us out
  platform->Init();
  gCodes->Init();
//webserver->Init();
  move->Init();
  heat->Init();
  currentTool = NULL;
  message[0] = 0;
  const uint32_t wdtTicks = 256;	// number of watchdog ticks @ 32768Hz/128 before the watchdog times out (max 4095)
//  WDT_Enable(WDT, (wdtTicks << WDT_MR_WDV_Pos) | (wdtTicks << WDT_MR_WDD_Pos) | WDT_MR_WDRSTEN);	// enable watchdog, reset the mcu if it times out
  coldExtrude = true;		// DC42 changed default to true for compatibility because for now we are aiming for compatibility with RRP 0.78
  active = true;			// must do this before we start the network, else the watchdog may time out

  platform->Message(HOST_MESSAGE, "%s Version %s dated %s\n", NAME, VERSION, DATE);
//  FileStore *startup = platform->GetFileStore(platform->GetSysDir(), platform->GetConfigFile(), false);

  platform->AppendMessage(HOST_MESSAGE, "\n\nExecuting ");
  if(0/*startup != NULL*/)
  {
//	  startup->Close();
	  platform->AppendMessage(HOST_MESSAGE, "%s...\n\n", platform->GetConfigFile());
	  scratchString.printf("M98 P%s\n", platform->GetConfigFile());
  }
  else
  {
	  platform->AppendMessage(HOST_MESSAGE, "%s (no configuration file found)...\n\n", platform->GetDefaultFile());
	  scratchString.printf("M98 P%s\n", platform->GetDefaultFile());
  }

  // We inject an M98 into the serial input stream to run the start-up macro

  platform->GetLine()->InjectString(scratchString.Pointer());

  bool runningTheFile = false;
  bool initialisingInProgress = true;
  while (initialisingInProgress)
  {
	  Spin();
	  if(gCodes->PrintingAFile())
	  {
		  runningTheFile = true;
	  }
	  if(runningTheFile)
	  {
		  if(!gCodes->PrintingAFile())
		  {
			  initialisingInProgress = false;
		  }
	  }
  }
  processingConfig = false;

/*  if (network->IsEnabled())
  {
	  platform->AppendMessage(HOST_MESSAGE, "\nStarting network...\n");
	  network->Init(); // Need to do this here, as the configuration GCodes may set IP address etc.
  }
  else
  {
	  platform->AppendMessage(HOST_MESSAGE, "\nNetwork disabled.\n");
  }*/

  platform->AppendMessage(HOST_MESSAGE, "\n%s is up and running.\n", NAME);
  fastLoop = FLT_MAX;
  slowLoop = 0.0;
  lastTime = platform->Time();
}

void RepRap::Exit()
{
  active = false;
  heat->Exit();
  move->Exit();
  gCodes->Exit();
//  webserver->Exit();
  platform->Message(HOST_MESSAGE, "RepRap class exited.\n");
  platform->Exit();
}

void RepRap::Spin()
{
	if(!active)
		return;

	spinningModule = modulePlatform;
	ticksInSpinState = 0;
	platform->Spin();

/*	spinningModule = moduleNetwork;
	ticksInSpinState = 0;
	network->Spin();

	spinningModule = moduleWebserver;
	ticksInSpinState = 0;
	webserver->Spin();
*/

	spinningModule = moduleGcodes;
	ticksInSpinState = 0;
	gCodes->Spin();

	spinningModule = moduleMove;
	ticksInSpinState = 0;
	move->Spin();

	spinningModule = moduleHeat;
	ticksInSpinState = 0;
	heat->Spin();

	spinningModule = noModule;
	ticksInSpinState = 0;

	// Update the print stats
	UpdatePrintProgress();

	// Keep track of the loop time

	double t = platform->Time();
	double dt = t - lastTime;
	if(dt < fastLoop)
	{
		fastLoop = dt;
	}
	if(dt > slowLoop)
	{
		slowLoop = dt;
	}
	lastTime = t;
}

void RepRap::Timing()
{
	platform->AppendMessage(BOTH_MESSAGE, "Slowest main loop (seconds): %f; fastest: %f\n", slowLoop, fastLoop);
	fastLoop = FLT_MAX;
	slowLoop = 0.0;
}

void RepRap::Diagnostics()
{
  platform->Diagnostics();				// this includes a call to our Timing() function
  move->Diagnostics();
  heat->Diagnostics();
  gCodes->Diagnostics();
//network->Diagnostics();
//webserver->Diagnostics();
}

// Turn off the heaters, disable the motors, and
// deactivate the Heat and Move classes.  Leave everything else
// working.

void RepRap::EmergencyStop()
{
	stopped = true;
	platform->SetAtxPower(false);		// turn off the ATX power if we can

	//platform->DisableInterrupts();

	Tool* tool = toolList;
	while(tool)
	{
		tool->Standby();
		tool = tool->Next();
	}

	heat->Exit();
	for(int8_t heater = 0; heater < HEATERS; heater++)
	{
		platform->SetHeater(heater, 0.0);
	}

	// We do this twice, to avoid an interrupt switching
	// a drive back on.  move->Exit() should prevent
	// interrupts doing this.

	for(int8_t i = 0; i < 2; i++)
	{
		move->Exit();
		for(int8_t drive = 0; drive < DRIVES; drive++)
		{
			platform->SetMotorCurrent(drive, 0.0);
			platform->Disable(drive);
		}
	}
}

void RepRap::SetDebug(Module m, bool enable)
{
	if (enable)
	{
		debug |= (1 << m);
	}
	else
	{
		debug &= ~(1 << m);
	}
	PrintDebug();
}

void RepRap::SetDebug(bool enable)
{
	debug = (enable) ? 0xFFFF : 0;
}

void RepRap::PrintDebug()
{
	if (debug != 0)
	{
		platform->Message(BOTH_MESSAGE, "Debugging enabled for modules:");
		for(uint8_t i=0; i<16;i++)
		{
			if (debug & (1 << i))
			{
				platform->AppendMessage(BOTH_MESSAGE, " %s", moduleName[i]);
			}
		}
		platform->AppendMessage(BOTH_MESSAGE, "\n");
	}
	else
	{
		platform->Message(BOTH_MESSAGE, "Debugging disabled\n");
	}
}

/*
 * The first tool added becomes the one selected.  This will not happen in future releases.
 */

void RepRap::AddTool(Tool* tool)
{
	if(toolList == NULL)
	{
		toolList = tool;
		currentTool = tool;
		tool->Activate(currentTool);
		return;
	}

	toolList->AddTool(tool);
	tool->UpdateExtruderAndHeaterCount(activeExtruders, activeHeaters);
}

void RepRap::SelectTool(int toolNumber)
{
	Tool* tool = toolList;

	while(tool)
	{
		if(tool->Number() == toolNumber)
		{
			tool->Activate(currentTool);
			currentTool = tool;
			return;
		}
		tool = tool->Next();
	}

	// Selecting a non-existent tool is valid.  It sets them all to standby.

	if(currentTool != NULL)
	{
		StandbyTool(currentTool->Number());
	}
	currentTool = NULL;

}

void RepRap::PrintTool(int toolNumber, StringRef& reply) const
{
	for(Tool *tool = toolList; tool != NULL; tool = tool->next)
	{
		if(tool->Number() == toolNumber)
		{
			tool->Print(reply);
			return;
		}
	}
	reply.copy("Attempt to print details of non-existent tool.\n");
}

void RepRap::StandbyTool(int toolNumber)
{
	Tool* tool = toolList;

	while(tool)
	{
		if(tool->Number() == toolNumber)
		{
			tool->Standby();
			if(currentTool == tool)
			{
				currentTool = NULL;
			}
			return;
		}
		tool = tool->Next();
	}

	platform->Message(BOTH_MESSAGE, "Attempt to standby a non-existent tool: %d.\n", toolNumber);
}

Tool* RepRap::GetTool(int toolNumber)
{
	Tool* tool = toolList;

	while(tool)
	{
		if(tool->Number() == toolNumber)
		{
			return tool;
		}
		tool = tool->Next();
	}
	return NULL; // Not an error
}

#if 0	// not used
Tool* RepRap::GetToolByDrive(int driveNumber)
{
	Tool* tool = toolList;

	while (tool)
	{
		for(uint8_t drive = 0; drive < tool->DriveCount(); drive++)
		{
			if (tool->Drive(drive) + AXES == driveNumber)
			{
				return tool;
			}
		}

		tool = tool->Next();
	}
	return NULL;
}
#endif

void RepRap::SetToolVariables(int toolNumber, float* standbyTemperatures, float* activeTemperatures)
{
	Tool* tool = toolList;

	while(tool)
	{
		if(tool->Number() == toolNumber)
		{
			tool->SetVariables(standbyTemperatures, activeTemperatures);
			return;
		}
		tool = tool->Next();
	}

	platform->Message(BOTH_MESSAGE, "Attempt to set variables for a non-existent tool: %d.\n", toolNumber);
}


void RepRap::Tick()
{
	if (active)
	{
		WDT_Restart(WDT);			// kick the watchdog
		if (!resetting)
		{
			platform->Tick();
			++ticksInSpinState;
			if (ticksInSpinState >= 20000)	// if we stall for 20 seconds, save diagnostic data and reset
			{
				resetting = true;
				for(uint8_t i = 0; i < HEATERS; i++)
				{
					platform->SetHeater(i, 0.0);
				}
				for(uint8_t i = 0; i < DRIVES; i++)
				{
					platform->Disable(i);
					// We can't set motor currents to 0 here because that requires interrupts to be working, and we are in an ISR
				}

				move->PrintCurrentDda();
				platform->SoftwareReset(SoftwareResetReason::stuckInSpin);
			}
		}
	}
}

// Get the JSON status response for the web server (or later for the M105 command).
// Type 1 is the ordinary JSON status response.
// Type 2 is the same except that static parameters are also included.
// Type 3 is the same but instead of static parameters we report print estimation values.
void RepRap::GetStatusResponse(StringRef& response, uint8_t type, bool forWebserver)
{
	char ch;

	// Machine status
	if (processingConfig)
	{
		// Reading the configuration file
		ch = 'C';
	}
	else if (IsStopped())
	{
		// Halted
		ch = 'H';
	}
	else if (gCodes->IsPausing())
	{
		// Pausing / Decelerating
		ch = 'D';
	}
	else if (gCodes->IsResuming())
	{
		// Resuming
		ch = 'R';
	}
	else if (gCodes->IsPaused())
	{
		// Paused / Stopped
		ch = 'S';
	}
	else if (gCodes->PrintingAFile())
	{
		// Printing
		ch = 'P';
	}
	else if (gCodes->DoingFileMacro() || !move->NoLiveMovement())
	{
		// Busy
		ch = 'B';
	}
	else
	{
		// Idle
		ch = 'I';
	}
	response.printf("{\"status\":\"%c\",\"coords\":{", ch);

	/* Coordinates */
	{
		float liveCoordinates[DRIVES + 1];
		if (currentTool != NULL)
		{
			const float *offset = currentTool->GetOffset();
			for (size_t i = 0; i < AXES; ++i)
			{
				liveCoordinates[i] += offset[i];
			}
		}
		move->LiveCoordinates(liveCoordinates);

		// Homed axes
		response.catf("\"axesHomed\":[%d,%d,%d]",
				(gCodes->GetAxisIsHomed(0)) ? 1 : 0,
				(gCodes->GetAxisIsHomed(1)) ? 1 : 0,
				(gCodes->GetAxisIsHomed(2)) ? 1 : 0);

		// Actual and theoretical extruder positions since power up, last G92 or last M23
		response.catf(",\"extr\":");		// announce actual extruder positions
		ch = '[';
		for (uint8_t extruder = 0; extruder < GetExtrudersInUse(); extruder++)
		{
			response.catf("%c%.1f", ch, liveCoordinates[AXES + extruder]);
			ch = ',';
		}

		// XYZ positions
		response.cat("],\"xyz\":");
		ch = '[';
		for (uint8_t axis = 0; axis < AXES; axis++)
		{
			response.catf("%c%.2f", ch, liveCoordinates[axis]);
			ch = ',';
		}
	}

	// Current tool number
	int toolNumber = (currentTool == NULL) ? 0 : currentTool->Number();
	response.catf("]},\"currentTool\":%d", toolNumber);

	/* Output - only reported once */
	{
		bool sendBeep = (beepDuration != 0 && beepFrequency != 0);
		bool sendMessage = (message[0]) && ((gCodes->HaveAux() && !forWebserver) || (!gCodes->HaveAux() && forWebserver));
		if (sendBeep || sendMessage)
		{
			response.cat(",\"output\":{");

			// Report beep values
			if (sendBeep)
			{
				response.catf("\"beepDuration\":%d,\"beepFrequency\":%d", beepDuration, beepFrequency);
				if (sendMessage)
				{
					response.cat(",");
				}

				beepFrequency = beepDuration = 0;
			}

			// Report message
			if (sendMessage)
			{
				response.cat("\"message\":");
				EncodeString(response, message, 2, false);

				message[0] = 0;
			}
			response.cat("}");
		}
	}

	/* Parameters */
	{
		// ATX power
		response.catf(",\"params\":{\"atxPower\":%d", platform->AtxPower() ? 1 : 0);

		// Cooling fan value
		float fanValue = (gCodes->CoolingInverted() ? 1.0 - platform->GetFanValue() : platform->GetFanValue());
		response.catf(",\"fanPercent\":%.2f", fanValue * 100.0);

		// Speed and Extrusion factors
		response.catf(",\"speedFactor\":%.2f,\"extrFactors\":", gCodes->GetSpeedFactor() * 100.0);
		for (uint8_t extruder = 0; extruder < GetExtrudersInUse(); extruder++)
		{
			response.catf("%c%.2f", (extruder == 0) ? '[' : ',', gCodes->GetExtrusionFactors()[extruder] * 100.0);
		}
		response.cat("]}");
	}

	// G-code reply sequence for webserver
	if (forWebserver)
	{
		response.catf(",\"seq\":%d", replySeq);

		// There currently appears to be no need for this one, so skip it
		//response.catf(",\"buff\":%u", webserver->GetGcodeBufferSpace());
	}

	/* Sensors */
	{
		response.cat(",\"sensors\":{");

		// Probe
		int v0 = platform->ZProbe();
		int v1, v2;
		switch (platform->GetZProbeSecondaryValues(v1, v2))
		{
			case 1:
				response.catf("\"probeValue\":\%d,\"probeSecondary\":[%d]", v0, v1);
				break;
			case 2:
				response.catf("\"probeValue\":\%d,\"probeSecondary\":[%d,%d]", v0, v1, v2);
				break;
			default:
				response.catf("\"probeValue\":%d", v0);
				break;
		}

		// Fan RPM
		response.catf(",\"fanRPM\":%d}", (unsigned int)platform->GetFanRPM());
	}

	/* Temperatures */
	{
		response.cat(",\"temps\":{");

		/* Bed */
#if HOT_BED != -1
		{
			response.catf("\"bed\":{\"current\":%.1f,\"active\":%.1f,\"state\":%d},",
					heat->GetTemperature(HOT_BED), heat->GetActiveTemperature(HOT_BED),
					heat->GetStatus(HOT_BED));
		}
#endif

		/* Heads */
		{
			response.cat("\"heads\":{\"current\":");

			// Current temperatures
			ch = '[';
			for (int8_t heater = E0_HEATER; heater < GetHeatersInUse(); heater++)
			{
				response.catf("%c%.1f", ch, heat->GetTemperature(heater));
				ch = ',';
			}

			// Active temperatures
			response.catf("],\"active\":");
			ch = '[';
			for (int8_t heater = E0_HEATER; heater < GetHeatersInUse(); heater++)
			{
				response.catf("%c%.1f", ch, heat->GetActiveTemperature(heater));
				ch = ',';
			}

			// Standby temperatures
			response.catf("],\"standby\":");
			ch = '[';
			for (int8_t heater = E0_HEATER; heater < GetHeatersInUse(); heater++)
			{
				response.catf("%c%.1f", ch, heat->GetStandbyTemperature(heater));
				ch = ',';
			}

			// Heater statuses (0=off, 1=standby, 2=active, 3=fault)
			response.cat("],\"state\":");
			ch = '[';
			for (int8_t heater = E0_HEATER; heater < GetHeatersInUse(); heater++)
			{
				response.catf("%c%d", ch, (int)heat->GetStatus(heater));
				ch = ',';
			}
		}
		response.cat("]}}");
	}

	// Time since last reset
	response.catf(",\"time\":%.1f", platform->Time());

	/* Extended Status Response */
	if (type == 2)
	{
		// Cold Extrude/Retract
		response.catf(",\"coldExtrudeTemp\":%1.f", ColdExtrude() ? 0 : HOT_ENOUGH_TO_EXTRUDE);
		response.catf(",\"coldRetractTemp\":%1.f", ColdExtrude() ? 0 : HOT_ENOUGH_TO_RETRACT);

		// Delta configuration
		response.catf(",\"geometry\":\"%s\"", move->IsDeltaMode() ? "delta" : "cartesian");

		// Machine name
		response.cat(",\"name\":");
		EncodeString(response, myName, 2, false);

		/* Probe */
		{
			const ZProbeParameters& probeParams = platform->GetZProbeParameters();

			// Trigger threshold
			response.catf(",\"probe\":{\"threshold\":%d", probeParams.adcValue);

			// Trigger height
			response.catf(",\"height\":%.2f", probeParams.height);

			// Type
			response.catf(",\"type\":%d}", platform->GetZProbeType());
		}

		/* Tool Mapping */
		{
			response.cat(",\"tools\":[");
			for(Tool *tool=toolList; tool != NULL; tool = tool->Next())
			{
				// Heaters
				response.cat("{\"heaters\":[");
				for(uint8_t heater=0; heater<tool->HeaterCount(); heater++)
				{
					response.catf("%d", tool->Heater(heater));
					if (heater < tool->HeaterCount() - 1)
					{
						response.cat(",");
					}
				}

				// Extruder drives
				response.cat("],\"drives\":[");
				for(uint8_t drive=0; drive<tool->DriveCount(); drive++)
				{
					response.catf("%d", tool->Drive(drive));
					if (drive < tool->DriveCount() - 1)
					{
						response.cat(",");
					}
				}

				// Do we have any more tools?
				if (tool->Next() != NULL)
				{
					response.cat("]},");
				}
				else
				{
					response.cat("]}");
				}
			}
			response.cat("]");
		}
	}
	else if (type == 3)
	{
		// Current Layer
		response.catf(",\"currentLayer\":%d", currentLayer);

		// Current Layer Time
		response.catf(",\"currentLayerTime\":%.1f", (lastLayerTime > 0.0) ? (platform->Time() - lastLayerTime) : 0.0);

		// Raw Extruder Positions
		response.cat(",\"extrRaw\":");		// announce the extruder positions
		ch = '[';
		for (size_t drive = 0; drive < reprap.GetExtrudersInUse(); drive++)		// loop through extruders
		{
			response.catf("%c%.1f", ch, gCodes->GetRawExtruderPosition(drive));
			ch = ',';
		}

		// Fraction of file printed
		response.catf("],\"fractionPrinted\":%.1f", (gCodes->PrintingAFile()) ? (gCodes->FractionOfFilePrinted() * 100.0) : 0.0);

		// First Layer Duration
		response.catf(",\"firstLayerDuration\":%.1f", firstLayerDuration);

		// First Layer Height
		response.catf(",\"firstLayerHeight\":%.2f", firstLayerHeight);

		// Print Duration
		response.catf(",\"printDuration\":%.1f", (printStartTime > 0.0) ? (platform->Time() - printStartTime) : 0.0);

		// Warm-Up Time
		response.catf(",\"warmUpDuration\":%.1f", warmUpDuration);

		/* Print Time Estimations */
		{
			// Based on file progress
			response.catf(",\"timesLeft\":{\"file\":%.1f", EstimateTimeLeft(0));

			// Based on filament usage
			response.catf(",\"filament\":%.1f", EstimateTimeLeft(1));

			// Based on layers
			response.catf(",\"layer\":%.1f}", EstimateTimeLeft(2));
		}
	}

	response.cat("}");
}

// Get the JSON status response for the web server or M105 command.
// Type 0 is the old-style webserver status response (we should be able to bet rid of this soon).
// Type 1 is the new-style webserver status response.
// Type 2 is the M105 S2 response, which is like the new-style status response but some fields are omitted.
// Type 3 is the M105 S3 response, which is like the M105 S2 response except that static values are also included.
// 'seq' is the response sequence number, if it is not -1 and we have a higher sequence number then we send the gcode response
void RepRap::GetLegacyStatusResponse(StringRef& response, uint8_t type, int seq) const
{
	if (type != 0)
	{
		// New-style status request
		// Send the printing/idle status
		char ch = (processingConfig) ? 'C'
					: (reprap.IsStopped()) ? 'S'
					: (gCodes->PrintingAFile()) ? 'P'
					: 'I';
		response.printf("{\"status\":\"%c\",\"heaters\":", ch);

		// Send the heater actual temperatures
		const Heat *heat = reprap.GetHeat();
		ch = '[';
		for (int8_t heater = 0; heater < reprap.GetHeatersInUse(); heater++)
		{
			response.catf("%c%.1f", ch, heat->GetTemperature(heater));
			ch = ',';
		}

		// Send the heater active temperatures
		response.catf("],\"active\":");
		ch = '[';
		for (int8_t heater = 0; heater < reprap.GetHeatersInUse(); heater++)
		{
			response.catf("%c%.1f", ch, heat->GetActiveTemperature(heater));
			ch = ',';
		}

		// Send the heater standby temperatures
		response.catf("],\"standby\":");
		ch = '[';
		for (int8_t heater = 0; heater < reprap.GetHeatersInUse(); heater++)
		{
			response.catf("%c%.1f", ch, heat->GetStandbyTemperature(heater));
			ch = ',';
		}

		// Send the heater statuses (0=off, 1=standby, 2=active)
		response.cat("],\"hstat\":");
		ch = '[';
		for (int8_t heater = 0; heater < reprap.GetHeatersInUse(); heater++)
		{
			response.catf("%c%d", ch, (int)heat->GetStatus(heater));
			ch = ',';
		}

		// Send XYZ positions
		float liveCoordinates[DRIVES];
		reprap.GetMove()->LiveCoordinates(liveCoordinates);
		const Tool* currentTool = reprap.GetCurrentTool();
		if (currentTool != NULL)
		{
			const float *offset = currentTool->GetOffset();
			for (size_t i = 0; i < AXES; ++i)
			{
				liveCoordinates[i] += offset[i];
			}
		}
		response.catf("],\"pos\":");		// announce the XYZ position
		ch = '[';
		for (int8_t drive = 0; drive < AXES; drive++)
		{
			response.catf("%c%.2f", ch, liveCoordinates[drive]);
			ch = ',';
		}

		// Send extruder total extrusion since power up, last G92 or last M23
		response.catf("],\"extr\":");		// announce the extruder positions
		ch = '[';
		for (int8_t drive = 0; drive < reprap.GetExtrudersInUse(); drive++)		// loop through extruders
		{
			response.catf("%c%.1f", ch, gCodes->GetRawExtruderPosition(drive));
			ch = ',';
		}
		response.cat("]");

		// Send the speed and extruder override factors
		response.catf(",\"sfactor\":%.2f,\"efactor\":", gCodes->GetSpeedFactor() * 100.0);
		const float *extrusionFactors = gCodes->GetExtrusionFactors();
		for (unsigned int i = 0; i < reprap.GetExtrudersInUse(); ++i)
		{
			response.catf("%c%.2f", (i == 0) ? '[' : ',', extrusionFactors[i] * 100.0);
		}
		response.cat("]");

		// Send the current tool number
		int toolNumber = (currentTool == NULL) ? 0 : currentTool->Number();
		response.catf(",\"tool\":%d", toolNumber);
	}
	else
	{
		// The old (deprecated) poll response lists the status, then all the heater temperatures, then the XYZ positions, then all the extruder positions.
		// These are all returned in a single vector called "poll".
		// This is a poor choice of format because we can't easily tell which is which unless we already know the number of heaters and extruders.
		// RRP reversed the order at version 0.65 to send the positions before the heaters, but we haven't yet done that.
		char c = (gCodes->PrintingAFile()) ? 'P' : 'I';
		response.printf("{\"poll\":[\"%c\",", c); // Printing
		for (int8_t heater = 0; heater < HEATERS; heater++)
		{
			response.catf("\"%.1f\",", reprap.GetHeat()->GetTemperature(heater));
		}
		// Send XYZ and extruder positions
		float liveCoordinates[DRIVES];
		reprap.GetMove()->LiveCoordinates(liveCoordinates);
		for (int8_t drive = 0; drive < DRIVES; drive++)	// loop through extruders
		{
			char ch = (drive == DRIVES - 1) ? ']' : ',';	// append ] to the last one but , to the others
			response.catf("\"%.2f\"%c", liveCoordinates[drive], ch);
		}
	}

	// Send the Z probe value
	int v0 = platform->ZProbe();
	int v1, v2;
	switch (platform->GetZProbeSecondaryValues(v1, v2))
	{
	case 1:
		response.catf(",\"probe\":\"%d (%d)\"", v0, v1);
		break;
	case 2:
		response.catf(",\"probe\":\"%d (%d, %d)\"", v0, v1, v2);
		break;
	default:
		response.catf(",\"probe\":\"%d\"", v0);
		break;
	}

	// Send fan RPM value
	response.catf(",\"fanRPM\":%u", (unsigned int)platform->GetFanRPM());

	// Send the home state. To keep the messages short, we send 1 for homed and 0 for not homed, instead of true and false.
	if (type != 0)
	{
		response.catf(",\"homed\":[%d,%d,%d]",
				(gCodes->GetAxisIsHomed(0)) ? 1 : 0,
				(gCodes->GetAxisIsHomed(1)) ? 1 : 0,
				(gCodes->GetAxisIsHomed(2)) ? 1 : 0);
	}
	else
	{
		response.catf(",\"hx\":%d,\"hy\":%d,\"hz\":%d",
				(gCodes->GetAxisIsHomed(0)) ? 1 : 0,
				(gCodes->GetAxisIsHomed(1)) ? 1 : 0,
				(gCodes->GetAxisIsHomed(2)) ? 1 : 0);
	}

	if (gCodes->PrintingAFile())
	{
		// Send the fraction printed
		response.catf(",\"fraction_printed\":%.4f", max<float>(0.0, gCodes->FractionOfFilePrinted()));
	}

	response.cat(",\"message\":");
	EncodeString(response, message, 2, false);

/*	if (type < 2)
	{
		response.catf(",\"buff\":%u", webserver->GetGcodeBufferSpace());	// send the amount of buffer space available for gcodes
	}

	if (type < 2 || (seq != -1 && replySeq > seq))
	{
		response.catf(",\"seq\":%u", replySeq);								// send the response sequence number

		// Send the response to the last command. Do this last because it is long and may need to be truncated.
		response.cat(",\"resp\":");
		EncodeString(response, GetGcodeReply().Pointer(), 2, true);
	}

	if (type == 3)
	{
		// Add the static fields. For now this is just geometry and the machine name, but other fields could be added e.g. axis lengths.
		response.catf(",\"geometry\":\"%s\",\"myName\":", move->IsDeltaMode() ? "delta" : "cartesian");
		EncodeString(response, myName, 2, false);
	}

	response.cat("}");
*/}

// Copy some parameter text, stopping at the first control character or when the destination buffer is full, and removing trailing spaces
void RepRap::CopyParameterText(const char* src, char *dst, size_t length)
{
	size_t i;
	for (i = 0; i + 1 < length && src[i] >= ' '; ++i)
	{
		dst[i] = src[i];
	}
	// Remove any trailing spaces
	while (i > 0 && dst[i - 1] == ' ')
	{
		--i;
	}
	dst[i] = 0;
}

// Encode a string in JSON format and append it to a string buffer, truncating it if necessary to leave the specified amount of room
void RepRap::EncodeString(StringRef& response, const char* src, size_t spaceToLeave, bool allowControlChars)
{
	response.cat("\"");
	size_t j = response.strlen();
	while (j + spaceToLeave + 2 <= response.Length())	// while there is room for a character and a trailing quote
	{
		char c = *src++;
		if (c == 0 || (c < ' ' && !allowControlChars))	// if null terminator or bad character
		{
			break;
		}
		char esc;
		switch (c)
		{
		case '\r':
			esc = 'r';
			break;
		case '\n':
			esc = 'n';
			break;
		case '\t':
			esc = 't';
			break;
		case '"':
			esc = '"';
			break;
		case '\\':
			esc = '\\';
			break;
		default:
			esc = 0;
			break;
		}
		if (esc)
		{
			if (j + spaceToLeave + 2 == response.Length())
			{
				break;					// if no room for the extra backslash then quit
			}
			response[j++] = '\\';
			response[j++] = esc;
		}
		else
		{
			response[j++] = c;
		}
	}
	response[j++] = '"';
	response[j] = 0;
}
/*
// Get just the machine name in JSON format
void RepRap::GetNameResponse(StringRef& response) const
{
	response.copy("{\"myName\":");
	EncodeString(response, myName, 2, false);
	response.cat("}");
}*/

// Get the list of files in the specified directory in JSON format
void RepRap::GetFilesResponse(StringRef& response, const char* dir) const
{
	response.copy("{\"files\":[");
	FileInfo file_info;
	bool firstFile = true;
	bool gotFile = platform->GetMassStorage()->FindFirst(dir, file_info);
	while (gotFile && response.strlen() + strlen(file_info.fileName) + 6 < response.Length())
	{
		if (!firstFile)
		{
			response.catf(",");
		}
		EncodeString(response, file_info.fileName, 3, false);

		firstFile = false;
		gotFile = platform->GetMassStorage()->FindNext(file_info);
	}
	response.cat("]}");
}

// Get information for the specified file, or the currently printing file, in JSON format
void RepRap::GetFileInfoResponse(StringRef& response, const char* filename) const
{
	// Poll file info for a specific file
	if (filename != NULL)
	{
	/*	GcodeFileInfo info;
		bool found = webserver->GetFileInfo("0:/", filename, info);
		if (found)
		{
			response.printf("{\"err\":0,\"size\":%lu,\"height\":%.2f,\"layerHeight\":%.2f,\"filament\":",
							info.fileSize, info.objectHeight, info.layerHeight);
			char ch = '[';
			if (info.numFilaments == 0)
			{
				response.catf("%c", ch);
			}
			else
			{
				for (unsigned int i = 0; i < info.numFilaments; ++i)
				{
					response.catf("%c%.1f", ch, info.filamentNeeded[i]);
					ch = ',';
				}
			}
			response.catf("],\"generatedBy\":\"%s\"}", info.generatedBy);
		}
		else */
		{
			response.copy("{\"err\":1}");
		}
	}
	else if (GetGCodes()->PrintingAFile() && fileInfoDetected)
	{
		// Poll file info about a file currently being printed
		response.printf("{\"err\":0,\"size\":%lu,\"height\":%.2f,\"layerHeight\":%.2f,\"filament\":",
						currentFileInfo.fileSize, currentFileInfo.objectHeight, currentFileInfo.layerHeight);
		char ch = '[';
		if (currentFileInfo.numFilaments == 0)
		{
			response.catf("%c", ch);
		}
		else
		{
			for (unsigned int i = 0; i < currentFileInfo.numFilaments; ++i)
			{
				response.catf("%c%.1f", ch, currentFileInfo.filamentNeeded[i]);
				ch = ',';
			}
		}
		response.catf("],\"generatedBy\":\"%s\",\"printDuration\":%d,\"fileName\":\"%s\"}",
				currentFileInfo.generatedBy, (int)((platform->Time() - printStartTime) * 1000.0), fileBeingPrinted);
	}
	else
	{
		response.copy("{\"err\":1}");
	}
}

void RepRap::StartingFilePrint(const char *filename)
{
//	fileInfoDetected = Webserver::GetFileInfo(platform->GetGCodeDir(), filename, currentFileInfo);
	printStartTime = platform->Time();
	strncpy(fileBeingPrinted, filename, ARRAY_SIZE(fileBeingPrinted));
	fileBeingPrinted[ARRAY_UPB(fileBeingPrinted)] = 0;
}

void RepRap::Beep(int freq, int ms)
{
	if (gCodes->HaveAux())
	{
		// If there is an LCD device present, make it beep
		platform->Beep(freq, ms);
	}
	else
	{
		// Otherwise queue it until the webserver can process it
		beepFrequency = freq;
		beepDuration = ms;
	}
}

void RepRap::SetMessage(const char *msg)
{
	strncpy(message, msg, maxMessageLength);
	message[maxMessageLength] = 0;
}

void RepRap::MessageToGCodeReply(const char *message)
{
	gcodeReply.copy(message);
	++replySeq;
}

void RepRap::AppendMessageToGCodeReply(const char *message)
{
	gcodeReply.cat(message);
}

void RepRap::AppendCharToStatusResponse(const char c)
{
	gcodeReply.catf("%c", c);
}

bool RepRap::NoPasswordSet() const
{
	return (!password[0] || StringEquals(password, DEFAULT_PASSWORD));
}

bool RepRap::CheckPassword(const char *pw) const
{
	return StringEquals(pw, password);
}

void RepRap::SetPassword(const char* pw)
{
	// Users sometimes put a tab character between the password and the comment, so allow for this
	CopyParameterText(pw, password, ARRAY_SIZE(password));
}

const char *RepRap::GetName() const
{
	return myName;
}

void RepRap::SetName(const char* nm)
{
	// Users sometimes put a tab character between the machine name and the comment, so allow for this
	CopyParameterText(nm, myName, ARRAY_SIZE(myName));
}

// The following methods keep track of the current print

void RepRap::UpdatePrintProgress()
{
	if (gCodes->IsPausing() || gCodes->IsPaused() || gCodes->IsResuming())
	{
		return;
	}

	if (gCodes->PrintingAFile())
	{
		// May have just started a print, see if we're heating up
		if (warmUpDuration == 0.0)
		{
			// When a new print starts, the total (raw) extruder positions are zeroed
			float totalRawFilament = 0.0;
			for (size_t extruder=0; extruder<DRIVES - AXES; extruder++)
			{
				totalRawFilament += gCodes->GetRawExtruderPosition(extruder);
			}

			// See if at least one heater is active and set
			bool heatersAtHighTemperature = false;
			for (uint8_t heater=E0_HEATER; heater<HEATERS; heater++)
			{
				if (heat->GetStatus(heater) == Heat::HS_active &&
					heat->GetActiveTemperature(heater) > TEMPERATURE_LOW_SO_DONT_CARE &&
					heat->HeaterAtSetTemperature(heater))
				{
					heatersAtHighTemperature = true;
					break;
				}
			}

			if (heatersAtHighTemperature && totalRawFilament != 0.0)
			{
				lastLayerTime = platform->Time();
				warmUpDuration = lastLayerTime - printStartTime;

				if (fileInfoDetected && currentFileInfo.layerHeight > 0.0) {
					currentLayer = 1;
				}
			}
		}
		// Looks like the print has started
		else if (currentLayer > 0)
		{
			float liveCoords[DRIVES + 1];
			move->LiveCoordinates(liveCoords);

			// See if we can determine the first layer height (must be smaller than the nozzle diameter)
			if (firstLayerHeight == 0.0)
			{
				if (liveCoords[Z_AXIS] < NOZZLE_DIAMETER && !gCodes->DoingFileMacro())
				{
					firstLayerHeight = liveCoords[Z_AXIS];
				}
			}
			// Then check if we've finished the first layer
			else if (firstLayerDuration == 0.0)
			{
				if (liveCoords[Z_AXIS] > firstLayerHeight * 1.05) // allow some tolerance for transform operations
				{
					firstLayerFilament = 0.0;
					for (size_t extruder=0; extruder<DRIVES - AXES; extruder++)
					{
						firstLayerFilament += gCodes->GetRawExtruderPosition(extruder);
					}
					firstLayerDuration = platform->Time() - lastLayerTime;
					firstLayerProgress = gCodes->FractionOfFilePrinted();
				}
			}
			// We have enough values to estimate the following layer heights
			else if (currentFileInfo.objectHeight > 0.0)
			{
				unsigned int estimatedLayer = round((liveCoords[Z_AXIS] - firstLayerHeight) / currentFileInfo.layerHeight) + 1;
				if (estimatedLayer == currentLayer + 1) // on layer change
				{
					// Record untainted extruder positions for filament-based estimation
					float extrRawTotal = 0.0;
					for(uint8_t extruder=0; extruder<DRIVES - AXES; extruder++)
					{
						extrRawTotal += gCodes->GetRawExtruderPosition(extruder);
					}

					const float now = platform->Time();
					unsigned int remainingLayers;
					remainingLayers = round((currentFileInfo.objectHeight - firstLayerHeight) / currentFileInfo.layerHeight) + 1;
					remainingLayers -= currentLayer;

					if (currentLayer > 1)
					{
						// Record a new set
						if (numLayerSamples < MAX_LAYER_SAMPLES)
						{
							layerDurations[numLayerSamples] = now - lastLayerTime;
							if (!numLayerSamples)
							{
								filamentUsagePerLayer[numLayerSamples] = extrRawTotal - firstLayerFilament;
							}
							else
							{
								filamentUsagePerLayer[numLayerSamples] = extrRawTotal - lastLayerFilament;
							}
							fileProgressPerLayer[numLayerSamples] = gCodes->FractionOfFilePrinted();
							numLayerSamples++;
						}
						else
						{
							for(unsigned int i=1; i<MAX_LAYER_SAMPLES; i++)
							{
								layerDurations[i - 1] = layerDurations[i];
								filamentUsagePerLayer[i - 1] = filamentUsagePerLayer[i];
								fileProgressPerLayer[i - 1] = fileProgressPerLayer[i];
							}

							layerDurations[MAX_LAYER_SAMPLES - 1] = now - lastLayerTime;
							filamentUsagePerLayer[MAX_LAYER_SAMPLES - 1] = extrRawTotal - lastLayerFilament;
							fileProgressPerLayer[MAX_LAYER_SAMPLES - 1] = gCodes->FractionOfFilePrinted();
						}
					}

					// Update layer-based estimation times
					float avgLayerTime, avgLayerDelta = 0.0;
					if (numLayerSamples)
					{
						avgLayerTime = 0.0;
						for(unsigned int layer=0; layer<numLayerSamples; layer++)
						{
							avgLayerTime += layerDurations[layer];
							if (layer)
							{
								avgLayerDelta += layerDurations[layer] - layerDurations[layer - 1];
							}
						}
						avgLayerTime /= numLayerSamples;
						avgLayerDelta /= numLayerSamples;
					}
					else
					{
						avgLayerTime = firstLayerDuration * FIRST_LAYER_SPEED_FACTOR;
					}

					layerEstimatedTimeLeft = (avgLayerTime * remainingLayers) - (avgLayerDelta * remainingLayers);
					if (layerEstimatedTimeLeft < 0.0)
					{
						layerEstimatedTimeLeft = avgLayerTime * remainingLayers;
					}

					// TODO: maybe move other estimation methods here too?
					// And move whole estimation code to a separate class?

					// Set new layer values
					currentLayer = estimatedLayer;
					lastLayerTime = now;
					lastLayerFilament = extrRawTotal;
				}
			}
		}
	}
	else if (printStartTime > 0.0 && move->NoLiveMovement())
	{
		currentLayer = numLayerSamples = 0;
		firstLayerDuration = firstLayerHeight = firstLayerFilament = firstLayerProgress = 0.0;
		layerEstimatedTimeLeft = printStartTime = warmUpDuration = 0.0;
		lastLayerTime = lastLayerFilament = 0.0;
	}
}

float RepRap::EstimateTimeLeft(uint8_t method) const
{
	// We can't provide an estimation if we're not printing (yet)
	if (!gCodes->PrintingAFile() || (fileInfoDetected && currentFileInfo.numFilaments && warmUpDuration == 0.0))
	{
		return 0.0;
	}

	// Take into account the first layer time only if we haven't got any other samples
	float realPrintDuration = (platform->Time() - printStartTime) - warmUpDuration;
	if (numLayerSamples)
	{
		realPrintDuration -= firstLayerDuration;
	}

	// Actual estimations
	switch (method)
	{
		case 0: // File-Based
		{
			// Provide rough estimation only if we haven't collected any layer samples
			float fractionPrinted = gCodes->FractionOfFilePrinted();
			if (!numLayerSamples || !fileInfoDetected || currentFileInfo.objectHeight == 0.0)
			{
				return realPrintDuration * (1.0 / fractionPrinted) - realPrintDuration;
			}

			// Each layer takes time to achieve more file progress, so take an average over our samples
			float avgSecondsByProgress = 0.0, lastLayerProgress = 0.0;
			for(unsigned int layer=0; layer<numLayerSamples; layer++)
			{
				avgSecondsByProgress += layerDurations[layer] / (fileProgressPerLayer[layer] - lastLayerProgress);
				lastLayerProgress = fileProgressPerLayer[layer];
			}
			avgSecondsByProgress /= numLayerSamples;

			// Then we know how many seconds it takes to finish 1% and we know how much file progress is left
			return avgSecondsByProgress * (1.0 - fractionPrinted);
		}

		case 1: // Filament-Based
		{
			// Need some file information, otherwise this method won't work
			if (!fileInfoDetected || !currentFileInfo.numFilaments)
			{
				return 0.0;
			}

			// Sum up the filament usage and the filament needed
			float totalFilamentNeeded = 0.0;
			float extrRawTotal = 0.0;
			for (size_t extruder=0; extruder < DRIVES - AXES; extruder++)
			{
				totalFilamentNeeded += currentFileInfo.filamentNeeded[extruder];
				extrRawTotal += gCodes->GetRawExtruderPosition(extruder);
			}

			// If we have a reasonable amount of filament extruded, calculate estimated times left
			if (totalFilamentNeeded > 0.0 && extrRawTotal > totalFilamentNeeded * ESTIMATION_MIN_FILAMENT_USAGE)
			{
				if (firstLayerFilament == 0.0)
				{
					return realPrintDuration * (totalFilamentNeeded - extrRawTotal) / extrRawTotal;
				}

				float filamentRate;
				if (numLayerSamples)
				{
					filamentRate = 0.0;
					for (unsigned int i=0; i<numLayerSamples; i++)
					{
						filamentRate += filamentUsagePerLayer[i] / layerDurations[i];
					}
					filamentRate /= numLayerSamples;
				}
				else
				{
					filamentRate = firstLayerFilament / firstLayerDuration;
				}

				return (totalFilamentNeeded - extrRawTotal) / filamentRate;
			}
			break;
		}

		case 2: // Layer-Based
			if (layerEstimatedTimeLeft > 0.0)
			{
				float timeLeft = layerEstimatedTimeLeft - (platform->Time() - lastLayerTime);
				if (timeLeft > 0.0)
				{
					return timeLeft;
				}
			}
			break;
	}

	return 0.0;
}

void RepRap::GetExtruderCapabilities(bool canDrive[], const bool directions[]) const
{
	for (uint8_t extruder=0; extruder<DRIVES - AXES; extruder++)
	{
		canDrive[extruder] = false;
	}

	Tool *tool = toolList;
	while (tool != nullptr)
	{
		for(uint8_t driveNum = 0; driveNum < tool->DriveCount(); driveNum++)
		{
			const int extruderDrive = tool->Drive(driveNum);
			canDrive[extruderDrive] = tool->ToolCanDrive(directions[extruderDrive + AXES] == FORWARDS);
		}

		tool = tool->Next();
	}
}

void RepRap::FlagTemperatureFault(int8_t dudHeater)
{
	if (toolList != NULL)
	{
		toolList->FlagTemperatureFault(dudHeater);
	}
}

void RepRap::ClearTemperatureFault(int8_t wasDudHeater)
{
	reprap.GetHeat()->ResetFault(wasDudHeater);
	if (toolList != NULL)
	{
		toolList->ClearTemperatureFault(wasDudHeater);
	}
}

//*************************************************************************************************
// StringRef class member implementations

size_t StringRef::strlen() const
{
	return strnlen(p, len - 1);
}

int StringRef::printf(const char *fmt, ...)
{
	va_list vargs;
	va_start(vargs, fmt);
	int ret = vsnprintf(p, len, fmt, vargs);
	va_end(vargs);
	return ret;
}

int StringRef::vprintf(const char *fmt, va_list vargs)
{
	return vsnprintf(p, len, fmt, vargs);
}

int StringRef::catf(const char *fmt, ...)
{
	size_t n = strlen();
	if (n + 1 < len)		// if room for at least 1 more character and a null
	{
		va_list vargs;
		va_start(vargs, fmt);
		int ret = vsnprintf(p + n, len - n, fmt, vargs);
		va_end(vargs);
		return ret + n;
	}
	return 0;
}

// This is quicker than printf for printing constant strings
size_t StringRef::copy(const char* src)
{
	size_t length = strnlen(src, len - 1);
	memcpy(p, src, length);
	p[length] = 0;
	return length;
}

// This is quicker than catf for printing constant strings
size_t StringRef::cat(const char* src)
{
	size_t length = strlen();
	size_t toCopy = strnlen(src, len - length - 1);
	memcpy(p + length, src, toCopy);
	length += toCopy;
	p[length] = 0;
	return length;
}

//*************************************************************************************************

// Utilities and storage not part of any class

static char scratchStringBuffer[100];		// this is now used only for short messages
StringRef scratchString(scratchStringBuffer, ARRAY_SIZE(scratchStringBuffer));

// For debug use
void debugPrintf(const char* fmt, ...)
{
	va_list vargs;
	va_start(vargs, fmt);
	reprap.GetPlatform()->Message(DEBUG_MESSAGE, fmt, vargs);
	va_end(vargs);
}

// String testing

bool StringEndsWith(const char* string, const char* ending)
{
  int j = strlen(string);
  int k = strlen(ending);
  if(k > j)
    return false;

  return(StringEquals(&string[j - k], ending));
}

bool StringEquals(const char* s1, const char* s2)
{
  int i = 0;
  while(s1[i] && s2[i])
  {
     if(tolower(s1[i]) != tolower(s2[i]))
       return false;
     i++;
  }

  return !(s1[i] || s2[i]);
}

bool StringStartsWith(const char* string, const char* starting)
{
  int j = strlen(string);
  int k = strlen(starting);
  if(k > j)
    return false;

  for(int i = 0; i < k; i++)
    if(string[i] != starting[i])
      return false;

  return true;
}

int StringContains(const char* string, const char* match)
{
  int i = 0;
  int count = 0;

  while(string[i])
  {
    if(string[i++] == match[count])
    {
      count++;
      if(!match[count])
        return i;
    } else
    {
      count = 0;
    }
  }

  return -1;
}












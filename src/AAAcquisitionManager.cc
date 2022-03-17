/////////////////////////////////////////////////////////////////////////////////
//                                                                             //
//                           Copyright (C) 2012-2016                           //
//                 Zachary Seth Hartwig : All rights reserved                  //
//                                                                             //
//      The ADAQAcquisition source code is licensed under the GNU GPL v3.0.    //
//      You have the right to modify and/or redistribute this source code      //
//      under the terms specified in the license, which may be found online    //
//      at http://www.gnu.org/licenses or at $ADAQACQUISITION/License.txt.     //
//                                                                             //
/////////////////////////////////////////////////////////////////////////////////

#include <TSystem.h>

#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <bitset>
#include <algorithm>
#include <cmath>

#include "AAAcquisitionManager.hh"
#include "AAVMEManager.hh"
#include "AAGraphics.hh"


AAAcquisitionManager *AAAcquisitionManager::TheAcquisitionManager = 0;


AAAcquisitionManager *AAAcquisitionManager::GetInstance()
{ return TheAcquisitionManager; }


AAAcquisitionManager::AAAcquisitionManager()
  : AcquisitionEnable(false), AcquisitionTimerEnable(false),
    AcquisitionTimeStart(0), AcquisitionTimeStop(0),
    AcquisitionTimeNow(0), AcquisitionTimePrev(0),
    UseSTDFirmware(true), UsePSDFirmware(false),
    AnalyzePSDList(true), AnalyzePSDWaveform(false),
    EventPointer(NULL), EventWaveform(NULL), Buffer(NULL),
    BufferSize(0), ReadSize(0), FPGAEvents(0), PCEvents(0),
    ReadoutType(0), ReadoutTypeBit(24), ReadoutTypeMask(0b1 << ReadoutTypeBit),
    ZLEEventSizeMask(0x0fffffff), ZLEEventSize(0),
    ZLESampleAMask(0x0000ffff), ZLESampleBMask(0xffff0000),
    ZLENumWordMask(0x000fffff), ZLEControlMask(0xc0000000),
    EventCounter(0),
    LLD(0), ULD(0), SampleHeight(0.), TriggerHeight(0.),
    PulseHeight(0.), PulseArea(0.), PSDTotal(0.), PSDTail(0.), PeakPosition(0),
    RawTimeStamp(0), RateAccum(0),
    FillWaveformTree(false), TheReadoutManager(new ADAQReadoutManager)
{
  if(TheAcquisitionManager)
    cout << "\nError! The AcquisitionManager was constructed twice!\n" << endl;
  TheAcquisitionManager = this;
}


AAAcquisitionManager::~AAAcquisitionManager()
{
  delete TheAcquisitionManager;
  delete TheReadoutManager;
}


void AAAcquisitionManager::Initialize()
{
  ADAQDigitizer *DGManager = AAVMEManager::GetInstance()->GetDGManager();

  Int_t DGChannels = DGManager->GetNumChannels();

  for(Int_t ch=0; ch<DGChannels; ch++){

    BufferFull.push_back(true);

    WaveformLength.push_back(0);

    BaselineStart.push_back(0);
    BaselineStop.push_back(0);
    BaselineLength.push_back(0);
    BaselineValue.push_back(0);

    Polarity.push_back(0.);

    PeakPosition.push_back(0);
    PSDTotalAbsStart.push_back(0);
    PSDTotalAbsStop.push_back(0);
    PSDTailAbsStart.push_back(0);
    PSDTailAbsStop.push_back(0);

    CalibrationDataStruct DataStruct;
    CalibrationData.push_back(DataStruct);
    CalibrationEnable.push_back(false);
    CalibrationCurves.push_back(new TGraph);

    Spectrum_H.push_back(new TH1F);
    SpectrumExists.push_back(true);

    // Rate_P.push_back(new TGraph);
    Rate_C.push_back(new std::list<unsigned int>(0));
    Rate_Lead.push_back(std::numeric_limits<double>::max());
    RateExists.push_back(true);

    PSDHistogram_H.push_back(new TH2F);
    PSDHistogramExists.push_back(true);

    NumPSDEvents.push_back(0);

    CorrectedTimeStamp.push_back(0);
    PrevTimeStamp.push_back(0);
    PrevCorTimeStamp.push_back(0);
    TimeStampRollovers.push_back(0);
  }
}


void AAAcquisitionManager::PrepareAcquisition()
{
  ADAQDigitizer *DGManager = AAVMEManager::GetInstance()->GetDGManager();

  Int_t NumDGChannels = DGManager->GetNumChannels();

  // Set CAEN firmware type class member booleans for easy use
  UseSTDFirmware = TheSettings->STDFirmware;
  UsePSDFirmware = TheSettings->PSDFirmware;

  // Set user-preference to analyze PSD list data or waveforms
  AnalyzePSDList = TheSettings->PSDListAnalysis;
  AnalyzePSDWaveform = TheSettings->PSDWaveformAnalysis;

  ////////////////////////////////////////////////////
  // Initialize general member data for acquisition //
  ////////////////////////////////////////////////////

  ////////////////////
  // Acquisition timer

  AcquisitionTimeNow = 0;
  AcquisitionTimePrev = 0;

  ///////////////////////
  // Baseline calculation

  for(Int_t ch=0; ch<NumDGChannels; ch++){

    if(UseSTDFirmware){
      BaselineStart[ch] = TheSettings->ChBaselineCalcMin[ch];
      BaselineStop[ch] = TheSettings->ChBaselineCalcMax[ch];
      BaselineLength[ch] = BaselineStop[ch] - BaselineStart[ch];
      BaselineValue[ch] = 0.;
    }
    else if(UsePSDFirmware){

      // Set the device-specific baseline calculation region

      ZBoardType DGType = DGManager->GetBoardType();

      Int_t BaselineSamples = 0;
      Int_t BaselineSelection = TheSettings->ChBaselineSamples[ch];

      if(DGType == zV1720 or DGType == zDT5720 or
	 DGType == zDT5790M or DGType == zDT5790N or DGType == zDT5790P){

	switch(BaselineSelection){
	case 1:
	  BaselineSamples = 8;
	  break;
	case 2:
	  BaselineSamples = 32;
	  break;
	case 3:
	  BaselineSamples = 128;
	  break;
	default:
	  break;
	}
      }
      else if(DGType == zV1725 or DGType == zDT5730){

	switch(BaselineSelection){
	case 1:
	  BaselineSamples = 16;
	  break;
	case 2:
	  BaselineSamples = 64;
	  break;
	case 3:
	  BaselineSamples = 256;
	  break;
	case 4:
	  BaselineSamples = 1024;
	  break;
	default:
	  break;
	}
      }

      // Use the baseline calculation region (fixed number of samples)
      // to specify the baseline start/stop times
      BaselineStop[ch] = TheSettings->ChPreTrigger[ch] - TheSettings->ChGateOffset[ch] - 1;
      BaselineStart[ch] = BaselineStop[ch] - BaselineSamples;
      BaselineLength[ch] = BaselineSamples;
      BaselineValue[ch] = 0.;
    }

    if(TheSettings->ChPosPolarity[ch])
      Polarity[ch] = 1.;
    else
      Polarity[ch] = -1.;
  }


  //////////////////////////
  // PSD integral calculation

  if(UsePSDFirmware){
    for(Int_t ch=0; ch<NumDGChannels; ch++){
      Int_t GateStart = TheSettings->ChPreTrigger[ch] - TheSettings->ChGateOffset[ch];
      PSDTotalAbsStart[ch] = PSDTailAbsStart[ch] = GateStart;
      PSDTotalAbsStop[ch] = GateStart + TheSettings->ChLongGate[ch];
      PSDTailAbsStop[ch] = GateStart + TheSettings->ChShortGate[ch];
    }
  }


  ///////////////////
  // Waveform readout

  // Zero suppression waveforms: All channels (outer vector) are
  // preallocated; the waveform vector (inner vector) memory is *not
  // preallocated* since length of the waveform is unknown a priori
  if(TheSettings->ZeroSuppressionEnable){
    Waveforms.clear();
    Waveforms.resize(NumDGChannels);

    Waveforms4Storage.clear();
    Waveforms4Storage.resize(NumDGChannels);
  }

  // Raw and data reduction waveforms : All digitizer channels (outer
  // vector) are preallocated; the waveform vector (inner vector)
  // memory *is preallocated* since each channel has fixed size
  else{

    Waveforms.clear();
    Waveforms.resize(NumDGChannels);

    Waveforms4Storage.clear();
    Waveforms4Storage.resize(NumDGChannels);

    for(Int_t ch=0; ch<NumDGChannels; ch++){

      if(TheSettings->ChEnable[ch]){

	if(UseSTDFirmware)
	  WaveformLength[ch] = TheSettings->RecordLength;
	else if(UsePSDFirmware)
	  WaveformLength[ch] = TheSettings->ChRecordLength[ch];

	if(TheSettings->DataReductionEnable)
	  WaveformLength[ch] /= TheSettings->DataReductionFactor;

	Waveforms[ch].resize(WaveformLength[ch]);
      }
      else
	Waveforms[ch].resize(0);
    }
  }

  WaveformData.clear();
  for(Int_t ch=0; ch<NumDGChannels; ch++)
    WaveformData.push_back(new ADAQWaveformData);


  /////////////////////////
  // Pulse spectra creation

  LLD = TheSettings->SpectrumLLD;
  ULD = TheSettings->SpectrumULD;

  // Create pulse spectra and PSD histogram objects

  for(Int_t ch=0; ch<NumDGChannels; ch++){

    if(SpectrumExists[ch]){
      delete Spectrum_H[ch];
      SpectrumExists[ch] = false;

      stringstream SS;
      SS << "Spectrum[Ch" << ch << "]";
      string Name = SS.str();

      Spectrum_H[ch] = new TH1F(Name.c_str(),
				Name.c_str(),
				TheSettings->SpectrumNumBins,
				TheSettings->SpectrumMinBin,
				TheSettings->SpectrumMaxBin);

      SpectrumExists[ch] = true;
    }

    if(PSDHistogramExists[ch]){
      delete PSDHistogram_H[ch];
      PSDHistogramExists[ch] = false;

      stringstream SS;
      SS << "PSDHistogram[Ch" << ch << "]";
      string Name = SS.str();

      PSDHistogram_H[ch] = new TH2F(Name.c_str(),
				    Name.c_str(),
				    TheSettings->PSDTotalBins,
				    TheSettings->PSDTotalMinBin,
				    TheSettings->PSDTotalMaxBin,
				    TheSettings->PSDTailBins,
				    TheSettings->PSDTailMinBin,
				    TheSettings->PSDTailMaxBin);

      PSDHistogramExists[ch] = true;
    }

  }


  // GraphicsManager settings

  // In order to maximize readout loop efficiency, any graphical
  // object settings that only need to be set a single time are
  // called once from this pre-acquisition method

  if(TheSettings->WaveformMode)
    AAGraphics::GetInstance()->SetupWaveformGraphics(WaveformLength);
  else if(TheSettings->SpectrumMode)
    AAGraphics::GetInstance()->SetupSpectrumGraphics();
  else if(TheSettings->PSDMode)
    AAGraphics::GetInstance()->SetupPSDHistogramGraphics();
  else if(TheSettings->RateMode){
    SetupRateVector();
    AAGraphics::GetInstance()->SetupRateGraphics();
  }

  for(Int_t ch=0; ch<NumDGChannels; ch++){
    NumPSDEvents[ch] = 0;

    // Reset time stamp variables
    TimeStampRollovers[ch] = 0;
    PrevTimeStamp[ch] = 0;
    PrevCorTimeStamp[ch] = 0;
    CorrectedTimeStamp[ch] = 0;
  }

  if(UseSTDFirmware){

    // Initialize pointers to the event and event waveform. Memory is
    // preallocated for events here rather than at readout time
    // resulting in slightly larger memory use but faster readout

    EventPointer = NULL;
    EventWaveform = NULL;
    DGManager->AllocateEvent(&EventWaveform);
  }
  else if(UsePSDFirmware){
    PSDWaveforms = NULL;
  }

  // Initialize variables for the PC buffer and event readout. Memory
  // is preallocated for the buffer to receive readout events.
  Buffer = NULL;
  BufferSize = ReadSize = FPGAEvents = PCEvents = EventCounter = 0;
  for(Int_t ch=0; ch<DGManager->GetNumChannels(); ch++)
    NumPSDEvents[ch] = 0;

  // Allocate memory for the PC readout buffer only after the
  // digitizer been completely programmed
  DGManager->MallocReadoutBuffer(&Buffer, &BufferSize);

  if(UsePSDFirmware){
    DGManager->MallocDPPEvents(PSDEvents, &PSDEventSize);
    DGManager->MallocDPPWaveforms(&PSDWaveforms, &PSDWaveformSize);
  }

  // Get the acquisition control setting
  Int_t AcqControl = TheSettings->AcquisitionControl;

  // If acquisition is 'standard' or 'manual' then send the software
  // (SW) signal to begin data acquisition
  if(AcqControl == 0)
    DGManager->SWStartAcquisition();

  // If acquisition is 'Gated (NIM/TTL)' then arm the digitizer for
  // reception of S IN signal as data acquisition start/stop
  else if(AcqControl == 1 or AcqControl == 2)
    DGManager->SInArmAcquisition();
}


void AAAcquisitionManager::StartAcquisition()
{
  unsigned int hys = 0;
  ADAQDigitizer *DGManager = AAVMEManager::GetInstance()->GetDGManager();

  AAGraphics *TheGraphicsManager = AAGraphics::GetInstance();

  // Prepare variables and the digitizer for data acquisitio
  PrepareAcquisition();

  // Start data acquisition
  AcquisitionEnable = true;

  ///////////////////////////////
  // The data acquisition loop //
  ///////////////////////////////

  // Display acquisition control register 0x8100
  uint32_t acq_contr_reg;
  DGManager->GetRegisterValue(0x8100, &acq_contr_reg);
  cout << "Register 0x8100: " << acq_contr_reg << endl;

  // Display time over/under threshold register 0x1n84
  uint32_t ov_und_reg;
  DGManager->GetRegisterValue(0x1084, &ov_und_reg);
  cout << "Register 0x1084: " << ov_und_reg << endl;

  while(AcquisitionEnable){

    // Handle the acquisition loop in a separate ROOT thread
    gSystem->ProcessEvents();

    // Extra check to prevent possible final loop in this thread after
    // stop acquisition command is issued
    if(!AcquisitionEnable)
      break;

    /////////////////////////////////
    // Event readout determination //
    /////////////////////////////////

    // Standard firmware readout

    if(UseSTDFirmware){

      // Get the number of events stored in digitizer FPGA
      DGManager->GetNumFPGAEvents(&FPGAEvents);

      // Proceed only if FPGA events exceeds user-specified readout
      // events in order to maximize efficiency
      if(FPGAEvents < TheSettings->EventsBeforeReadout and
	 TheSettings->AcquisitionControl == 0)
	continue;

      // Transfer data from FPGA buffer to PC buffer
      DGManager->ReadData(Buffer, &ReadSize);

      // Get the total number of events in the PC buffer
      DGManager->GetNumEvents(Buffer, ReadSize, &PCEvents);
    }

    // DPP-PSD firmware readout

    else if(UsePSDFirmware){

      // Transfer data from FPGA buffer to PC buffer
      DGManager->ReadData(Buffer, &ReadSize);

      // The returned value of ReadData indicates data transfer status:
      //  = 0 : FPGA events < specified readout events; no transfer occured
      //  > 0 : FPGA events >= specified events require; tranfser occured

      // If no events were transferred then continue waiting for data
      if(ReadSize == 0)
      	continue;

      // Readout events from PC buffer to DPP-PSD event structure
      DGManager->GetDPPEvents(Buffer, ReadSize, PSDEvents, &NumPSDEvents[0]);
    }

    //////////////////////////////
    // Event data readout loops //
    //////////////////////////////

    // In order to accomodate both CAEN STD and DPP-PSD firmware,
    // which handle event aggregation differently, the nested
    // hierarchy of the readout loop is as follows:
    //
    //   for(all enabled channels)
    //     for(all channel-specific events)
    //       for(waveform samples) {only if settings require}

    // Loop over the digitizer readout channels
    for(Int_t ch=0; ch<DGManager->GetNumChannels(); ch++){

      // Skip channels that are not enabled for efficiency
      if(!TheSettings->ChEnable[ch])
	continue;

      // If DPP-PSD, get number of events in present channel
      if(UsePSDFirmware)
	PCEvents = NumPSDEvents[ch];

      // Reset all channel's corrected time stamp values to -42 to
      // ensure time stamps only register for the triggered channel
      CorrectedTimeStamp[ch] = 0;

      // Loop over the digitizer stored events in the PC buffer
      for(Int_t evt=0; evt<PCEvents; evt++){

	////////////////////////////
	// Pre-event-readout actions

	// Reset event-level variables
	BaselineValue[ch] = PulseHeight = PulseArea = 0.;
	PSDTotal = PSDTail = 0.;
	WaveformData[ch]->Initialize();

	if(AcquisitionTimerEnable){

	  // Calculate the elapsed time since the timer was started
	  AcquisitionTimePrev = AcquisitionTimeNow;
	  AcquisitionTimeNow = time(NULL) - AcquisitionTimeStart; // [seconds]

	  // Update the AQTimer widget only every second
	  if(AcquisitionTimePrev != AcquisitionTimeNow){
	    Int_t TimeRemaining = AcquisitionTimeStop - AcquisitionTimeNow;
	    TheInterface->UpdateAQTimer(TimeRemaining);
	  }

	  // If the timer is zero then stop acquisition; make sure to
	  // 'return' to completely escape the acquisition loop
	  if(AcquisitionTimeNow >= AcquisitionTimeStop){
	    StopAcquisition();
	    return;
	  }
	}

	/////////////////////////////
	// Event and waveform readout

	// Perform CAEN standard and DPP-PSD waveform readout

	if(!TheSettings->ZeroSuppressionEnable){

	  // Perform standard firmware event and waveform readout

	  if(UseSTDFirmware){

	    // Fill the EventInfo structure with waveform data
	    EventPointer = NULL;
	    DGManager->GetEventInfo(Buffer, ReadSize, evt, &EventInfo, &EventPointer);

	    // Segmentation fault protection
	    if(EventPointer == NULL){
	      continue;
	    }

	    //  Fill the EventWaveform structure with the digitized waveform
	    DGManager->DecodeEvent(EventPointer, &EventWaveform);

	    // Segmentation fault protection
	    if(EventWaveform == NULL)
	      continue;
	  }

	  // Perform DPP-PSD firmware waveform readout

	  else if(UsePSDFirmware and AnalyzePSDWaveform){

	    // Segmentation fault protection for using the acquisition
	    // timer. Timing can get out of sync at shut-down so this
	    // check prevents the decoding events when memory has
	    // already been freed to prevent crash.
	    if(AcquisitionEnable)
	      DGManager->DecodeDPPWaveforms(&PSDEvents[ch][evt], PSDWaveforms);
	    else
	      break;
	  }
	}

	// Perform CAEN standard firmware zero suppression waveform readout

	else{

	  // Use ADAQDigitizer method to readout ZLE waveform directly
	  // from the PC buffer into the Waveforms data member
	  Bool_t ZLESuccess = DGManager->GetZLEWaveform(Buffer, evt, Waveforms);

	  if(ZLESuccess != 0){
	    cout << "\nAAAcquisitionManager::StartAcquisition() : You've encountered a serious error!\n"
		 <<   "  There was an error reading out Event[" << evt << "] when using ZLE mode!\n"
		 <<   "  This issue is likely due to using a RecordLength > 4030. This setting causes\n"
		 <<   "  -- CAEN_DGTZ_GetNumEvents() to incorrectly return a '1'\n"
		 <<   "  -- The readout PC buffer is not correctly filled causing algorithm to segfault\n"
		 <<   "  CAEN has been contacted regarding this bug. ZSH (16 Oct 14)\n"
		 << endl;

	    continue;
	  }
	  //DGManager->PrintZLEEventInfo(Buffer, evt);
	}

	///////////////////////////////////
	// Post-readout waveform processing

	// Readout and process full waveforms for STD firwmare; do the
	// same for PSD firmware in 'Oscilloscope' mode (all
	// digitizers) or 'Mixed' modes (V1720/DT5790)

	if(UseSTDFirmware or (UsePSDFirmware and AnalyzePSDWaveform)){

	  // Get the number of samples in the current waveform
	  uint32_t NumSamples = 0;
	  if(UseSTDFirmware)
	    NumSamples = Waveforms[ch].size();
	  else if(UsePSDFirmware)
	    NumSamples = PSDWaveforms->Ns;

	  for(uint32_t sample=0; sample<NumSamples; sample++){

	    // Store raw and data-reduction waveforms into the waveforms
	    // data member; zero-suppression waveforms are already
	    // stored at this point in the acquisition loop

	    if(!TheSettings->ZeroSuppressionEnable){

	      // Data reduction waveforms
	      if(TheSettings->DataReductionEnable){
          if(sample % TheSettings->DataReductionFactor == 0){
            Int_t Index = sample / TheSettings->DataReductionFactor;
            if(UseSTDFirmware)
              Waveforms[ch][Index] = EventWaveform->DataChannel[ch][sample];
            else if(UsePSDFirmware)
              Waveforms[ch][Index] = PSDWaveforms->Trace1[sample];
          }
	      }

	      // Raw waveforms
	      else{
          if(UseSTDFirmware)
            Waveforms[ch][sample] = EventWaveform->DataChannel[ch][sample];
          else if(UsePSDFirmware)
            Waveforms[ch][sample] = PSDWaveforms->Trace1[sample];
	      }
	    }

	    if(!TheSettings->DisplayNonUpdateable){

	      // Calculate the baseline by taking the average of all
	      // samples that fall within the baseline calculation region
	      if(sample > BaselineStart[ch] and sample <= BaselineStop[ch])
          BaselineValue[ch] += Waveforms[ch][sample] * 1.0 / BaselineLength[ch]; // [ADC]

	      // Analyze the pulses to obtain pulse spectra
	      else if(sample >= BaselineStop[ch]){

          // Calculate the waveform sample distance from the baseline
          SampleHeight = Polarity[ch] * (Waveforms[ch][sample] - BaselineValue[ch]);

          // Simple algorithm to determine the pulse height [ADC]
          // and peak position [sample] by looping over all samples
          if(SampleHeight > PulseHeight){
            PulseHeight = SampleHeight;
            PeakPosition[ch] = sample;
          }

          // Integrate the "area under the pulse" by summing the
          // all samples in the waveform. Note that the assumption
          // is made that + and - noise will cancel
          PulseArea += SampleHeight;
	      }
	    }
	  }// End sample loop

	  // Computation of PSD integrals

	  // In STD firmware, the PSD integrals are taken relative to
	  // the peak position in time so the integrals must be taken
	  // *after* the sample loop, in which the peak position is
	  // determined, concludes. The PSD firmware values are fixed
	  // and set in AAAcquisitionManager::PreAcquisition()

	  // Set the PSD integral limits in units of absolute sample number
	  if(UseSTDFirmware){
	    PSDTotalAbsStart[ch] = PeakPosition[ch] + TheSettings->ChPSDTotalStart[ch];
	    PSDTotalAbsStop[ch] = PeakPosition[ch] + TheSettings->ChPSDTotalStop[ch];
	    PSDTailAbsStart[ch] = PeakPosition[ch] + TheSettings->ChPSDTailStart[ch];
	    PSDTailAbsStop[ch] = PeakPosition[ch] + TheSettings->ChPSDTailStop[ch];
	  }

	  // Only take the time to compute PSD integrals if necessary
	  if(TheSettings->PSDMode or TheSettings->WaveformStorePSDData){

	    // The total PSD integral
	    Int_t sample = PSDTotalAbsStart[ch];
	    for(; sample<PSDTotalAbsStop[ch]; sample++)
	      PSDTotal += Polarity[ch] * (Waveforms[ch][sample] - BaselineValue[ch]);

	    // The tail PSD integral
	    sample = PSDTailAbsStart[ch];
	    for(; sample<PSDTailAbsStop[ch]; sample++)
	      PSDTail += Polarity[ch] * (Waveforms[ch][sample] - BaselineValue[ch]);

	    // If running CAEN's DPP-PSD firmware and analyzing full
	    // waveforms then convert CAEN's "short integral" (the
	    // non-standard convention of gate offset to the end of
	    // the short integral) to "tail integral" (the standard
	    // integral from mid-pulse to the end of the waveform).

	    if(UsePSDFirmware)
	      PSDTail = PSDTotal - PSDTail;
	  }
	} // End STD or PSD waveform analysis

	// Analyze PSD list mode data

	if(UsePSDFirmware and AnalyzePSDList){

	  // Baseline returned in "Mixed" mode, == 0 in "List" mode
	  BaselineValue[ch] = PSDEvents[ch][evt].Baseline;

	  // Readout the DPP-PSD computed on the digitizer FPGA. Two
	  // deails are important to note:
	  //
	  // (1) The PSD integrals are recast from signed 16-bit
	  // integers (a maximum useable value of 2**15 == 32768) into
	  // unsigned 16-bit integers (a maximum value of 2**16 ==
	  // 65536) to maximize the energy resolution by maximizing
	  // the available channels that can accomodate the dynamic
	  // range of the digitizer input
	  //
	  // (2) Convert CAEN's "short integral" (the non-standard
	  // convention of gate offset to the end of the short
	  // integral) to the "tail integral" (the standard integral
	  // from mid-pulse to the end of the waveform). See below.

	  // The PSD long integral
	  PSDTotal = (UShort_t)PSDEvents[ch][evt].ChargeLong;

	  // The PSD short integral
	  PSDTail = PSDTotal - (UShort_t)PSDEvents[ch][evt].ChargeShort;

	  // Set the PSD long integral to the pulse area
	  PulseArea = PSDTotal;
	}

	/////////////////////////////////////////
	// Trigger time stamp rollover correction

	// Correct the time stamp for rollover. For STD firmware, the
	// timestamp is stored in bits [31:1] of a 32-bit unsigned
	// integer and requires a bit shift before operating on. For
	// PSD firmware, the returned 32 bit integer does not require
	// a bit shift. The formula for accounting for timestamp
	// rollover is:
	//
	//   Corrected = Raw + Rollovers * 2**31
	//
	// where:
	//  - 'Corrected' is the 64-bit rollover-corrected time stamp
	//  - 'Raw' is the unmodified 31-bit time stamp
	//  - 'Rollovers' is the number of rollovers that have occured

	// Get the raw time stamp
	if(UseSTDFirmware)
	  RawTimeStamp = (EventInfo.TriggerTimeTag >> 1);
	else if(UsePSDFirmware)
	  RawTimeStamp = (PSDEvents[ch][evt].TimeTag);

	// Test the time stamp for a rollover and increment if found
	if(RawTimeStamp < PrevTimeStamp[ch])
	{
          // std::cout<<"Rolling "<<CorrectedTimeStamp[ch]<<" "<<RawTimeStamp<<" "<<PrevTimeStamp[ch]<<"\n";
	  TimeStampRollovers[ch]++;
        }

	// Compute the corrected time stamp; store as 64-bit integer
  PrevCorTimeStamp[ch] = CorrectedTimeStamp[ch];
	CorrectedTimeStamp[ch] = (ULong64_t)(RawTimeStamp + TimeStampRollovers[ch] * ((ULong64_t)1<<DGManager->GetTimeStampSize()));// pow(2,32));

	// Set the previous time stamp
	PrevTimeStamp[ch] = RawTimeStamp;


	////////////////////////////
	// Post-readout data storage

	// First, we set the most basic information about the waveform
	// that we want to ensure is always stored in the ADAQ file
	// regardless of acquisition mode
	WaveformData[ch]->SetChannelID(ch);
	WaveformData[ch]->SetBoardID(DGManager->GetBoardID());
	WaveformData[ch]->SetTimeStamp(CorrectedTimeStamp[ch]);

	// Second, if the user has NOT selected the "nonupdatable
	// (ultra rate)" mode, we perform a number of digital pulse
	// processing and analyzed data storage steps. In order to
	// maximize the acquisition loop performance in ultra rate
	// mode, such things as pulse height/area, PSD integrals, and
	// other operations are NOT allowed.

	if(!TheSettings->DisplayNonUpdateable){

	  WaveformData[ch]->SetBaseline(BaselineValue[ch]);

	  // Store pulse area/height data and baseline if specified
	  if(TheSettings->WaveformStoreEnergyData){
	    WaveformData[ch]->SetPulseArea(PulseArea);
	    WaveformData[ch]->SetPulseHeight(PulseHeight);
	  }
	  // Store the total and tail PSD integrals if specified
	  if(TheSettings->WaveformStorePSDData){
	    WaveformData[ch]->SetPSDTotalIntegral(PSDTotal);
	    WaveformData[ch]->SetPSDTailIntegral(PSDTail);
	  }


	  ////////////////////////////////////////////
	  // Handle calibration for live-time analysis

	  // Note that calibration of pulse area/height data occurs
	  // after the energy data has been saved to the WaveformData
	  // class. This ensures that uncalibrated energy data is
	  // written to the ADAQ file for later processing.

	  if(CalibrationEnable[ch]){
	    if(TheSettings->SpectrumPulseHeight)
	      PulseHeight = CalibrationCurves[ch]->Eval(PulseHeight);
	    else
	      PulseArea = CalibrationCurves[ch]->Eval(PulseArea);
	  }

	  /////////////////////////////////////////
	  // Post-readout graphical object handling

	  if(TheSettings->SpectrumMode){

	    // Pulse height spectrum
	    if(TheSettings->SpectrumPulseHeight){

	      // Determine if the pulse height is within the
	      // acceptable lower/upper-level discrimator range if the
	      // user has specified this check on spectrum binning;
	      // otherwise, simply bin the pulse height in the spectrum

	      if(TheSettings->LDEnable){
          if(PulseHeight > LLD and PulseHeight < ULD)
            Spectrum_H[ch]->Fill(PulseHeight);
	      }
	      else
          Spectrum_H[ch]->Fill(PulseHeight);

	      // If the level-discrimantor is to be used as a
	      // 'trigger' to output the waveform to the ADAQ
	      if(TheSettings->LDTrigger and ch == TheSettings->LDChannel)
          FillWaveformTree = true;
	    }

	    // Pulse area spectrum
	    else{

	      // If using the level discriminator, determine if the
	      // pulse area is within the acceptable lower/upper-level
	      // discrimator range

	      if(TheSettings->LDEnable){
          if(PulseArea > LLD and PulseArea < ULD)
            Spectrum_H[ch]->Fill(PulseArea);
	      }

	      // If reading out waveforms with DPP-PSD in list mode,
	      // the maxium useful value is 2**16-1; prevent filling
	      // the Spectrum with these values

	      else if(UsePSDFirmware and TheSettings->PSDListAnalysis){
          if(PulseArea < pow(2,16)-1)
            Spectrum_H[ch]->Fill(PulseArea);
	      }

	      else
          Spectrum_H[ch]->Fill(PulseArea);

	      if(TheSettings->LDTrigger and ch == TheSettings->LDChannel)
          FillWaveformTree = true;
	    }
	  }

	  else if(TheSettings->PSDMode){
	    if(PSDTotal > TheSettings->PSDThreshold){

	      // The Y-axis value of the PSD histogram is the 'PSD
	      // parameter', which it typically the tail integral or
	      // the ratio of tail divided by the total integral

	      Double_t PSDParameter = PSDTail;
	      if(TheSettings->PSDYAxisTailTotal)
          PSDParameter /= PSDTotal;

	      PSDHistogram_H[ch]->Fill(PSDTotal, PSDParameter);
	    }
	  }

    else if(TheSettings->RateMode){
      Double_t tss = ((Double_t)CorrectedTimeStamp[ch])*DGManager->GetTimeStampUnit()*1e-9;

      if (PrevCorTimeStamp[ch]>CorrectedTimeStamp[ch]) cout<<"BACK IN TIME "<<PrevCorTimeStamp[ch]<<" "<<CorrectedTimeStamp[ch]<<"\n";
      // std::cout<<tss<<" "<<Rate_C[ch]->size()<<" "<<Rate_Lead[ch]<<" "<<TheSettings->RateNumPeriods<<"\n";

      // First event initialization
      if (Rate_Lead[ch] == std::numeric_limits<double>::max())
        Rate_Lead[ch] = tss;

      // Current plot vector index
      UInt_t tvi = (tss-Rate_Lead[ch])/TheSettings->RateIntegrationPeriod;

      // Increase size of storage list if needed
      if(tvi>=(double)Rate_C[ch]->size()){
        Rate_C[ch]->resize(tvi+1,0);
        RateAccum++;
      }

      // Timestamps are not ordered, so may need to iterate to correct bin (OK,
      // turns out they are, but this shouldn't cause a slowdown and maintains
      // generality for the case that they may not be)
      UInt_t diff = (Rate_C[ch]->size()-1) - tvi;
      UInt_t dcnt = 0;
      for (std::list<unsigned int>::reverse_iterator it=Rate_C[ch]->rbegin(); it != Rate_C[ch]->rend(); ++it, dcnt++)
        if (dcnt == diff){
          (*it)++;
          break;
        }

      // Trim front of container if beyond number of requested periods and
      // corresponding increment the list start time
      while(Rate_C[ch]->size()>TheSettings->RateNumPeriods){
        Rate_C[ch]->pop_front();
        Rate_Lead[ch]+=TheSettings->RateIntegrationPeriod;
      }

      // std::cout<<tss<<" "<<Rate_C[ch]->size()<<" "<<Rate_Lead[ch]<<" "<<TheSettings->RateNumPeriods<<"\n\n";
    }
	}

	///////////////////////////////////////
	// Post-readout data persistent storage

	if(TheSettings->WaveformStorageEnable){

	  // Skip this waveform if the pulse area/height does not fall
	  // within the discrimnator window (LLD to ULD).
	  if(TheSettings->LDEnable and !FillWaveformTree)
	    continue;

	  // Skip this waveform if readout is using DPP-PSD list mode
	  // and the pulse area is exceeds maximum useful value
	  if(UsePSDFirmware and TheSettings->PSDListAnalysis)
	    if(PulseArea > pow(2,16)-2)
	      continue;

	  // If storing raw waveforms to disk then copy the read out
	  // waveforms vector ("Waveforms") to the vector whose
	  // address is assigned to the waveforms branch in the ROOT
	  // TTree in the ADAQ file ("Waveforms4Storage"). While this
	  // is slightly inefficient (i.e. having two
	  // vector-of-vectors with the same data), it enables the
	  // user to analyze and view the waveforms without having to
	  // store them to disk. I am making the assumption that if
	  // the user is storing full waveforms then they understand
	  // the throughput penalty necessary to do this. Thus, the
	  // small amount of CPU time required for copying the vectors
	  // should be negligible.

	  if(TheSettings->WaveformStoreRaw){
	    for(int ch=0; ch<DGManager->GetNumChannels(); ch++){
	      Waveforms4Storage[ch] = Waveforms[ch];
	    }
	  }

	  // If the user has specified to store ANY data at all then
	  // fill the waveform tree via the readout manager
	  if(TheSettings->WaveformStoreRaw or
	     TheSettings->WaveformStoreEnergyData or
	     TheSettings->WaveformStorePSDData)
	    TheReadoutManager->GetWaveformTree()->Fill();

	  // Reset the bool used to determine if the LLD/ULD window
	  // should be used as the "trigger" for writing waveforms
	  FillWaveformTree = false;
	}

	/////////////////////////////////
	// Post-readout waveform plotting

	// Plot the waveform under specific criterion to minimize CPU
	// intensity. Only plot the waveforms in continuous data
	// acquisition mode and only plot the first waveform event in
	// the case of many events in a single readout.

	if(TheSettings->WaveformMode){

	  if(TheSettings->DisplayContinuous and evt == 0){

	    if(UseSTDFirmware or (UsePSDFirmware and AnalyzePSDWaveform)){

	      // Draw the digitized waveform
	      TheGraphicsManager->PlotWaveforms(Waveforms, WaveformLength);

	      // Draw graphical objects associated with the waveform
	      TheGraphicsManager->DrawWaveformGraphics(BaselineValue,
						       PeakPosition,
						       PSDTotalAbsStart,
						       PSDTotalAbsStop,
						       PSDTailAbsStart,
						       PSDTailAbsStop);
	    }
	  }
	}
	EventCounter++;

      } // End of the data readout loop over events

      // ZSH (23 Jul 15) : It is not clear to me why the following
      // reset of PSD event counter variable is needed. The value
      // should be set automatically during readout from the
      // ADAQDigitizer::GetDPPEvents() method at the top of the
      // acquisition loop. The reset is needed to prevent looping over
      // previously readout events but why...?

      // Zero the number of of PSD events after each channel readout.
      if(UsePSDFirmware)
        NumPSDEvents[ch] = 0;

    }// End of the data readout loop over channels


    /////////////////////////////////////
    // Post-data readout loop plotting //
    /////////////////////////////////////

    // Plot spectra or PSD histograms at certain event points if the
    // display is set to "continuous mode"; if in "updateable mode",
    // the "Update display" text button must be clicked for plotting

    if(TheSettings->DisplayContinuous){
      Int_t Rate = TheSettings->SpectrumRefreshRate;

      if(TheSettings->SpectrumMode){
        if(EventCounter % Rate == 0)
          TheGraphicsManager->PlotSpectrum(Spectrum_H[TheSettings->SpectrumChannel]);
      }

      else if(TheSettings->RateMode){
        if(EventCounter % Rate == 0 && RateAccum>1){ // Only plot after 2 points have been accumulated to avoid partial plots
          TheGraphicsManager->PlotRate(Rate_Lead[TheSettings->RateChannel]);
          RateAccum = 0;
        }
      }

      else if(TheSettings->PSDMode){
        if(EventCounter % Rate == 0){
          TheGraphicsManager->PlotPSDHistogram(PSDHistogram_H[TheSettings->PSDChannel]);
        }
      }
    }
  } // End of the acquisition loop
}


void AAAcquisitionManager::StopAcquisition()
{
  ADAQDigitizer *DGManager = AAVMEManager::GetInstance()->GetDGManager();

  Int_t AcqControl = TheSettings->AcquisitionControl;

  if(AcqControl == 0)
    DGManager->SWStopAcquisition();
  else if(AcqControl == 1 or AcqControl == 2)
    DGManager->SInDisarmAcquisition();

  AcquisitionEnable = false;

  DGManager->FreeReadoutBuffer(&Buffer);

  if(UseSTDFirmware){
    DGManager->FreeEvent(&EventWaveform);
  }
  else if(UsePSDFirmware){
    DGManager->FreeDPPEvents((void **)PSDEvents);
    DGManager->FreeDPPWaveforms(PSDWaveforms);
  }

  if(AcquisitionTimerEnable){

    // Set the information in the ADAQ file to signal that the
    // acquisition timer was used for this run and set the acquisition
    // time. This must be done here at the end of acquisition Since
    // when the ADAQReadoutInformation class is filled - when the file
    // _created_ not when the acquisition timer button is pushed - the
    // acquisition timer boolean has not yet been set.

    ADAQReadoutInformation *ARI = TheReadoutManager->GetReadoutInformation();
    ARI->SetAcquisitionTimer(AcquisitionTimerEnable);
    ARI->SetAcquisitionTime(TheSettings->AcquisitionTime);

    AcquisitionTimerEnable = false;
    TheInterface->UpdateAfterAQTimerStopped(TheReadoutManager->GetADAQFileOpen());
  }

  if(TheReadoutManager->GetADAQFileOpen())
    CloseADAQFile();
}


void AAAcquisitionManager::SaveObjectData(string ObjectType,
					  string FileName)
{
  Int_t Channel = TheSettings->SpectrumChannel;

  if(!SpectrumExists[Channel] or !PSDHistogramExists[Channel])
    return;

  size_t Found = FileName.find_last_of(".");
  if(Found != string::npos){

    // Get the file extension
    string FileExtension = FileName.substr(Found, string::npos);

    // The object (waveform, spectrum, PSD histogram) will be output
    // in a form factor that depends on the file extension:
    //   .dat: A columnar ASCII file
    //   .csv: a columnar CSV file
    //   .root: ROOT object is written to disk with the name:
    //          -> "Waveform" - Digitized waveform (TH1F)
    //          -> "Spectrum" - Pulse spectrum (TH1F)
    //          -> "PSDHistogram" - PSD histogram (TH2D)

    if(FileExtension == ".dat" or FileExtension == ".csv"){

      if(TheSettings->ObjectSaveWithTimeExtension){
	time_t Time = time(NULL);
	stringstream SS;
	SS << "." << Time;
	string TimeString = SS.str();

	FileName.insert(Found, TimeString);
      }

      // Create an ofstream object to write the data to a file
      ofstream ObjectOutput(FileName.c_str(), ofstream::trunc);

      // Assign the data separator based on file extension
      string Separator;
      if(FileExtension == ".dat")
	Separator = "\t";
      else if(FileExtension == ".csv")
	Separator = ",";

      if(ObjectType == "Waveform"){
	cout << "\nAAAcquisitionManager::SaveObjectData() : Waveform output is not yet implemented!\n"
	     << endl;
      }
      else if(ObjectType == "Spectrum"){

	// Get the number of bins in the spectrum histogram
	Int_t NumBins = Spectrum_H[Channel]->GetNbinsX();

	// Iterate over all the bins in spectrum histogram and output
	// the bin center (value on the X axis of the histogram) and the
	// bin content (value on the Y axis of the histogram)
	for(Int_t bin=1; bin<=NumBins; bin++){
	  Double_t BinCenter = Spectrum_H[Channel]->GetBinCenter(bin);
	  Double_t BinContent = Spectrum_H[Channel]->GetBinContent(bin);

	  ObjectOutput << BinCenter << Separator << BinContent
		       << endl;
	}
      }
      else if(ObjectType == "PSDHistogram"){
	Int_t NumXBins = PSDHistogram_H[Channel]->GetNbinsX();
	Int_t NumYBins = PSDHistogram_H[Channel]->GetNbinsY();

	Int_t XMin = PSDHistogram_H[Channel]->GetXaxis()->GetXmin();
	Int_t XMax = PSDHistogram_H[Channel]->GetXaxis()->GetXmax();
	Int_t YMin = PSDHistogram_H[Channel]->GetYaxis()->GetXmin();
	Int_t YMax = PSDHistogram_H[Channel]->GetYaxis()->GetXmax();

	ObjectOutput << setw(10) << NumXBins << setw(10) << XMin << setw(10) << XMax << "\n"
		     << setw(10) << NumYBins << setw(10) << YMin << setw(10) << YMax << "\n"
		     << endl;

	for(Int_t xbin=1; xbin<=NumXBins; xbin++){
	  for(Int_t ybin=1; ybin<=NumYBins; ybin++){
	    Int_t Bin = NumYBins*xbin + ybin;
	    ObjectOutput << PSDHistogram_H[Channel]->GetBinContent(Bin)
			 << endl;
	  }
	}
      }

      // Close the ofstream object
      ObjectOutput.close();
    }
    else if(FileExtension == ".root"){

      TFile *ObjectOutput = new TFile(FileName.c_str(), "recreate");

      if(ObjectType == "Waveform"){
      }

      else if(ObjectType == "Spectrum")
        Spectrum_H[Channel]->Write("Spectrum");

      else if(ObjectType == "PSDHistogram")
        PSDHistogram_H[Channel]->Write("PSDHistogram");

      ObjectOutput->Close();
    }
  }
}


Bool_t AAAcquisitionManager::AddCalibrationPoint(Int_t Channel, Int_t SetPoint,
						 Double_t Energy, Double_t PulseUnit)
{
  if(SetPoint == CalibrationData[Channel].PointID.size()){

    // Push data into the calibration data vectors
    CalibrationData[Channel].PointID.push_back(SetPoint);
    CalibrationData[Channel].Energy.push_back(Energy);
    CalibrationData[Channel].PulseUnit.push_back(PulseUnit);

    // Automatically sort the vectors from lowest-to-highest
    sort(CalibrationData[Channel].Energy.begin(),
	 CalibrationData[Channel].Energy.end());

    sort(CalibrationData[Channel].PulseUnit.begin(),
	 CalibrationData[Channel].PulseUnit.end());
  }
  else{
    CalibrationData[Channel].Energy[SetPoint] = Energy;
    CalibrationData[Channel].PulseUnit[SetPoint] = PulseUnit;
  }

  return true;
}


Bool_t AAAcquisitionManager::EnableCalibration(Int_t Channel)
{
  // If there are 2 or more points in the current channel's
  // calibration data set then create a new TGraph object. The
  // TGraph object will have pulse units [ADC] on the X-axis and the
  // corresponding energies [in whatever units the user has entered
  // the energy] on the Y-axis. A TGraph is used because it provides
  // very easy but extremely powerful methods for interpolation,
  // which allows the pulse height/area to be converted in to energy
  // efficiently in the acquisition loop.

  if(CalibrationData[Channel].PointID.size() >= 2){

    // Determine the total number of calibration points in the
    // current channel's calibration data set
    const Int_t NumPoints = CalibrationData[Channel].PointID.size();

    // Create a new "CalibrationManager" TGraph object.
    CalibrationCurves[Channel] = new TGraph(NumPoints,
					    &CalibrationData[Channel].PulseUnit[0],
					    &CalibrationData[Channel].Energy[0]);

    // Set the current channel's calibration boolean to true,
    // indicating that the current channel will convert pulse units
    // to energy within the acquisition loop before histogramming
    // the result into the channel's spectrum
    CalibrationEnable[Channel] = true;

    return true;
  }
  else
    return false;
}


Bool_t AAAcquisitionManager::ResetCalibration(Int_t Channel)
{
  // Clear the channel calibration vectors for the current channel
  CalibrationData[Channel].PointID.clear();
  CalibrationData[Channel].Energy.clear();
  CalibrationData[Channel].PulseUnit.clear();

  // Delete the current channel's depracated calibration manager
  // TGraph object to prevent memory leaks
  if(CalibrationEnable[Channel])
    delete CalibrationCurves[Channel];

  // Set the current channel's calibration boolean to false,
  // indicating that the calibration manager will NOT be used within
  // the acquisition loop
  CalibrationEnable[Channel] = false;

  return true;
}


Bool_t AAAcquisitionManager::LoadCalibration(Int_t Channel,
					   string FileName,
					   Int_t &NumPoints)
{
  size_t Found = FileName.find_last_of(".");
  if(Found != string::npos){

    // Set the calibration file to an input stream
    ifstream In(FileName.c_str());

    // An index to control the set point
    Int_t SetPoint = 0;

    // Iterate through each line in the file and use the values
    // (column1 == energy, column2 == pulse unit) to set the
    // CalibrationData objects for the current channel
    while(In.good()){
      Double_t Energy, PulseUnit;
      In >> Energy >> PulseUnit;

      if(In.eof()) break;

      AddCalibrationPoint(Channel, SetPoint, Energy, PulseUnit);

      SetPoint++;
    }

    NumPoints = SetPoint;

    return true;
  }
  else
    return false;
}


Bool_t AAAcquisitionManager::WriteCalibration(Int_t Channel, string FileName)
{
  // Set the calibration file to an input stream
  ofstream Out(FileName.c_str());

  for(Int_t i=0; i<CalibrationData[Channel].Energy.size(); i++)
    Out << setw(10) << CalibrationData[Channel].Energy[i]
	<< setw(10) << CalibrationData[Channel].PulseUnit[i]
	<< endl;
  Out.close();

  return true;
}


void AAAcquisitionManager::CreateADAQFile(string FileName)
{
  if(TheReadoutManager->GetADAQFileOpen())
    return;

  // Create a new ADAQ file via the readout manager
  TheReadoutManager->CreateFile(FileName);

  ADAQDigitizer *DGManager = AAVMEManager::GetInstance()->GetDGManager();

  Int_t DGChannels = DGManager->GetNumChannels();
  for(Int_t ch=0; ch<DGChannels; ch++){

    // For each digitizer channel, create the two mandatory TTree branches:
    // -A branch to store the channel's digitized waveform
    // -A branch to store analyzed waveform data in

    TheReadoutManager->CreateWaveformTreeBranches(ch,
						  &Waveforms4Storage[ch],
						  WaveformData[ch]);
  }

  // Get the pointer to the ADAQ readout information and fill with all
  // relevent information via the ADAQReadoutInformation::Set*() methods

  ADAQReadoutInformation *ARI = TheReadoutManager->GetReadoutInformation();

  // Set physical information about the digitizer device

  ARI->SetDGModelName      (DGManager->GetBoardModelName());
  ARI->SetDGSerialNumber   (DGManager->GetBoardSerialNumber());
  ARI->SetDGNumChannels    (DGManager->GetNumChannels());
  ARI->SetDGBitDepth       (DGManager->GetNumADCBits());
  ARI->SetDGSamplingRate   (DGManager->GetSamplingRate());
  ARI->SetDGROCFWRevision  (DGManager->GetBoardROCFirmwareRevision());
  ARI->SetDGAMCFWRevision  (DGManager->GetBoardAMCFirmwareRevision());
  if(TheSettings->STDFirmware)
    ARI->SetDGFWType       ("Standard");
  else if(TheSettings->PSDFirmware)
    ARI->SetDGFWType       ("DPP-PSD");

  // Fill global acquisition settings

  ARI->SetTriggerType          (TheSettings->TriggerTypeName);
  ARI->SetTriggerEdge          (TheSettings->TriggerEdgeName);
  ARI->SetAcquisitionType      (TheSettings->AcquisitionControlName);
  ARI->SetDataReductionMode    (TheSettings->DataReductionEnable);
  ARI->SetZeroSuppressionMode  (TheSettings->ZeroSuppressionEnable);
  ARI->SetCoincidenceLevel     (TheSettings->TriggerCoincidenceLevel);

  // Fill firmware-agnostic channel-specific settings

  ARI->SetChannelEnable    (TheSettings->ChEnable);
  ARI->SetDCOffset         (TheSettings->ChDCOffset);
  ARI->SetTrigger          (TheSettings->ChTriggerThreshold);
  ARI->SetBaselineCalcMin  (BaselineStart);
  ARI->SetBaselineCalcMax  (BaselineStop);
  if(TheSettings->STDFirmware){
    ARI->SetPSDTotalStart    (TheSettings->ChPSDTotalStart);
    ARI->SetPSDTotalStop     (TheSettings->ChPSDTotalStop);
    ARI->SetPSDTailStart     (TheSettings->ChPSDTailStart);
    ARI->SetPSDTailStop      (TheSettings->ChPSDTailStop);
  }
  else if(TheSettings->PSDFirmware){
    ARI->SetPSDTotalStart    (PSDTotalAbsStart);
    ARI->SetPSDTotalStop     (PSDTotalAbsStop);
    ARI->SetPSDTailStart     (PSDTailAbsStart);
    ARI->SetPSDTailStop      (PSDTailAbsStop);
  }

  // Fill CAEN standard firmware specific settings

  if(TheSettings->STDFirmware){
    ARI->SetRecordLength     (TheSettings->RecordLength);
    ARI->SetPostTrigger      (TheSettings->PostTrigger);

    ARI->SetZLEFwd           (TheSettings->ChZLEForward);
    ARI->SetZLEBck           (TheSettings->ChZLEBackward);
    ARI->SetZLEThreshold     (TheSettings->ChZLEThreshold);
  }

  // Fill CAEN DPP-PSD firmware specific settings

  else if(TheSettings->PSDFirmware){
    ARI->SetChRecordLength       (TheSettings->ChRecordLength);
    ARI->SetChChargeSensitivity  (TheSettings->ChChargeSensitivity);
    ARI->SetChPSDCut             (TheSettings->ChPSDCut);
    ARI->SetChTriggerConfig      (TheSettings->ChTriggerConfig);
    ARI->SetChTriggerValidation  (TheSettings->ChTriggerValidation);
    ARI->SetChShortGate          (TheSettings->ChShortGate);
    ARI->SetChLongGate           (TheSettings->ChLongGate);
    ARI->SetChPreTrigger         (TheSettings->ChPreTrigger);
    ARI->SetChGateOffset         (TheSettings->ChGateOffset);
  }

  // Fill information regarding waveform acquisition

  ARI->SetStoreRawWaveforms  (TheSettings->WaveformStoreRaw);
  ARI->SetStoreEnergyData    (TheSettings->WaveformStoreEnergyData);
  ARI->SetStorePSDData       (TheSettings->WaveformStorePSDData);
}


void AAAcquisitionManager::CloseADAQFile()
{
  if(!TheReadoutManager->GetADAQFileOpen())
    return;

  TheReadoutManager->WriteFile();
}

void AAAcquisitionManager::SetupRateVector()
{
  TheSettings->RateNumPeriods = (int)(TheSettings->RateDisplayPeriod/TheSettings->RateIntegrationPeriod);
  RateAccum = 0;
  for(Int_t ch=0; ch<AAVMEManager::GetInstance()->GetDGManager()->GetNumChannels(); ch++){
    // Rate_C[ch]->reserve(TheSettings->RateNumPeriods);
    Rate_Lead[ch] = std::numeric_limits<double>::max();
    Rate_C[ch]->clear();
  }
}

/*
void AAInterface::GenerateArtificialWaveform(Int_t RecordLength, vector<int> &Voltage,
						 Double_t *Voltage_graph, Double_t VerticalPosition)
{
  // Exponential time constants with small random variations
  const Double_t t1 = 20. - (RNG->Rndm()*10);
  const Double_t t2 = 80. - (RNG->Rndm()*40);;

  // The waveform amplitude with small random variations
  const Double_t a = RNG->Rndm() * 30;

  // Set an artificial baseline for the pulse
  const Double_t b = 3200;

  // Set an artifical polarity for the pulse
  const Double_t p = -1.0;

  // Set the number of leading zeros before the waveform begins with
  // small random variations
  const Int_t NumLeadingSamples = 100;

  // Fill the Voltage vector with the artificial waveform
  for(Int_t sample=0; sample<RecordLength; sample++){

    if(sample < NumLeadingSamples){
      Voltage[sample] = b + VerticalPosition;
      Voltage_graph[sample] = b + VerticalPosition;
    }
    else{
      Double_t t = (sample - NumLeadingSamples)*1.0;
      Voltage[sample] = (p * (a * (t1-t2) * (exp(-t/t1)-exp(-t/t2)))) + b + VerticalPosition;
      Voltage_graph[sample] = Voltage[sample];
    }
  }
}
*/

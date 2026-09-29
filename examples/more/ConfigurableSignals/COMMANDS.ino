// blaeck has already checked that the value is within [1, MAXIMUM_SIGNALS] and stored it
// before these run, so an out-of-range bound never reaches the sketch. They only save it.
void onSignalFirst()
{
  EEPROM.put(EEPROM_ADDR_SIGNAL_FIRST, signalFirst);
  EepromCommit();
}

void onSignalLast()
{
  EEPROM.put(EEPROM_ADDR_SIGNAL_LAST, signalLast);
  EepromCommit();
}

// The bounds SignalFirst and SignalLast hold. The "all signals" presets pass their own range.
void onSignalActivate()
{
  ApplySignalRange(true, signalFirst, signalLast);
}

void onSignalDeactivate()
{
  ApplySignalRange(false, signalFirst, signalLast);
}

// Applies the bounds it is given. Only the signals inside the range change, so
// activating 1-10 and then 15-20 leaves both ranges on - use the deactivate
// button to clear what you no longer want.
void ApplySignalRange(bool activate, byte lo, byte hi)
{
  if (lo > hi)
  {
    byte tmp = lo;
    lo = hi;
    hi = tmp;
  }
  if (lo < 1)
    lo = 1;
  if (hi > MAXIMUM_SIGNALS)
    hi = MAXIMUM_SIGNALS;

  for (byte i = lo; i <= hi; i++)
  {
    sine[i].isActivated = activate;
  }

  PersistActivatedSignals();
  UpdateLoggingSignals();
}

void PersistActivatedSignals()
{
  bool isActivated[MAXIMUM_SIGNALS + 1];
  for (byte i = 0; i <= MAXIMUM_SIGNALS; i++)
  {
    isActivated[i] = sine[i].isActivated;
  }
  EEPROM.put(EEPROM_ADDR_SIGNAL_ACTIVATED, isActivated);
  EepromCommit();
}

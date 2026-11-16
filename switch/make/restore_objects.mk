# Root (2026-11-15): rules for six objects that native/Makefile never compiled itself. They were built once by the stage
# pipelines (stage24 calendar clock, stage26 timebase, stage28 exitspawn, stage29 module admission, stage30 sysutil
# registry) and reused IN PLACE from build/startup-spurs-attributes-continuation-stage33-r1 (N12_REUSED), whose objects
# were deleted on 2026-11-14. Same sources and the same flag form as the lwcond.o rule those pipelines copied.
# Used only with N12_REUSED="" (build_lle_nro_sibling.sh reads this file after the others).
$(BUILD)/calendar_clock.o: $(SDK)/runtime/time/calendar_clock.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/calendar_clock_horizon.o: adapters/calendar_clock_horizon.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/timebase.o: $(SDK)/runtime/time/timebase.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/exitspawn_callbacks.o: $(SDK)/runtime/process/exitspawn_callbacks.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/module_admission.o: diagnostics/module_admission.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/sysutil_callback_registry.o: $(SDK)/runtime/sysutil/callback_registry.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

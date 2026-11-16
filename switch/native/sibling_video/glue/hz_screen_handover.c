/*
 * hz_screen_handover - who owns the Switch screen: the text status console or the Vulkan window.
 *
 * horizon/src/hz_window.c (vendored, unmodified) calls hz_screen_handover(1) when the backend opens its window and
 * hz_screen_handover(2) when it drops it. In the sibling's own program (horizon/src/hz_entry.c) that asks the main thread,
 * which owns a libnx console, to give the screen up / take it back, and waits up to 2 s.
 * In this program the status console is native/switch_main.c's, and the two calls the renderer needs already exist there:
 *     void native_status_release(void);   consoleExit under the status mutex (a no-op while the console is off)
 *     void native_status_begin(void);     consoleInit again (a no-op while the console is on); stdout/stderr stay the log
 * Both take g_status_mutex themselves, so they are safe from the thread that drives the renderer (the audio ticker), the
 * way rsx_render_bridge.c already calls them. No waiting is needed: they act directly.
 */
extern void native_status_begin(void);
extern void native_status_release(void);

void hz_screen_handover(int request)
{
    if (request == 1) native_status_release();
    else if (request == 2) native_status_begin();
}

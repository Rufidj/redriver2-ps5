"""The frame clock of the game: an accurate vblank thread, and a limiter whose interval config.ini sets.
usage: patch_clock.py PsyX_main.cpp OUT_PsyX_main.cpp main.c OUT_main.cpp"""
import sys

s = open(sys.argv[1]).read()
a = s.index('int intrThreadMain(void* data)')
b = s.index('static int PsyX_Sys_InitialiseCore()')
new_thread = r'''static double VblNowMs() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
int intrThreadMain(void* data)
{
	// vblanks on absolute deadlines: a sleep per vblank drifts (SDL_Delay(1) made 57 Hz out of 60)
	double next = VblNowMs();
	while (!g_stopIntrThread)
	{
		const double stepMs = (g_vmode == MODE_NTSC ? FIXED_TIME_STEP_NTSC : FIXED_TIME_STEP_PAL) * 1000.0;
		const double now = VblNowMs();
		if (now >= next)
		{
			SDL_LockMutex(g_intrMutex);
			if (vsync_callback)
				vsync_callback();
			SDL_UnlockMutex(g_intrMutex);
			g_psxSysCounters[PsxCounter_VBLANK]++;
			next += stepMs;
			if (now - next > stepMs * 4.0)
				next = now;
		}
		else
		{
			const double rem = next - now;
			if (rem > 0.7)
			{
				struct timespec ts = { 0, (long)((rem - 0.5) * 1e6) };
				nanosleep(&ts, NULL);
			}
		}
	}
	return 0;
}

'''
s = s[:a] + '#include <time.h>\n' + new_thread + s[b:]
open(sys.argv[2], 'w').write(s)

m = open(sys.argv[3]).read()
old = '''	// always stay 30 FPS (2 vblanks)
	if (VSync(-1) - frame < 2)
		return 0;'''
assert old in m
m = m.replace(old, '''	// 2 vblanks = 30 FPS; config.ini's frameInterval sets it
	extern int g_cfg_frameInterval;
	if (VSync(-1) - frame < g_cfg_frameInterval)
		return 0;''')
open(sys.argv[4], 'w').write(m)

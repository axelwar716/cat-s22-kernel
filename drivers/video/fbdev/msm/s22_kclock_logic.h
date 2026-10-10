/*
 * CAT S22 Flip cover clock (s22_kclock.c): pure logic, no kernel calls.
 * Kept apart so the host harness (cat-s22-rom/tools/kclock-harness) builds
 * exactly this code: local time and DST, text formatting, the autonomous
 * screen and the cover ownership plan.  Needs s22_kclock_font.h first and
 * div_s64(), div_u64_rem(), mktime64(), time64_to_tm(), struct tm,
 * DIV_ROUND_CLOSEST(), ARRAY_SIZE(), memset().
 */
#ifndef S22_KCLOCK_LOGIC_H
#define S22_KCLOCK_LOGIC_H

/* Colours (RGB565): the clock white; the date and battery S22Cover's
 * Color.LTGRAY (0xCCCCCC).  The "-white" build sets KC_SMALL_RGB to 0xffffff. */
#define KC_SMALL_RGB	0xcccccc
#define KC_RGB565(rgb)	((((rgb) >> 8) & 0xf800) | (((rgb) >> 5) & 0x07e0) | \
			 (((rgb) >> 3) & 0x001f))
#define KC_CLOCK_COLOR	0xffff
#define KC_SMALL_COLOR	KC_RGB565(KC_SMALL_RGB)

static int kc_weekday(time64_t t)
{
	return ((u32)div_s64(t, 86400) + 4) % 7;	/* 0 = Sunday */
}

/* UTC instant of hour:00 on the nth Sunday of mon (nth 0 = last). */
static time64_t kc_sunday(int year, int mon, int nth, int hour)
{
	static const u8 mdays[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30,
				      31, 30, 31 };
	int day;

	if (nth > 0)
		day = 1 + (7 - kc_weekday(mktime64(year, mon, 1, 0, 0, 0))) % 7 +
			(nth - 1) * 7;
	else
		day = mdays[mon] -
			kc_weekday(mktime64(year, mon, mdays[mon], 0, 0, 0));
	return mktime64(year, mon, day, hour, 0, 0);
}

/* 1 if t is in summer time: EU 01:00 UTC last Sunday Mar-Oct; US 02:00
 * local second Sunday Mar - first Sunday Nov (std = standard offset). */
static int kc_summer(int rule, time64_t t, int std)
{
	struct tm tm;
	int y;

	time64_to_tm(t, 0, &tm);
	y = tm.tm_year + 1900;
	if (rule == 1)
		return t >= kc_sunday(y, 3, 0, 1) && t < kc_sunday(y, 10, 0, 1);
	return t >= kc_sunday(y, 3, 2, 2) - std &&
		t < kc_sunday(y, 11, 1, 2) - std - 3600;
}

/*
 * Local time for the autonomous clock: now + off (sys_tz, which Android
 * sets at boot, on zone changes and at local midnight, DST included) plus
 * the DST hour Android has not applied yet since it set the zone at `set`.
 * rule: -1 auto (EU for UTC+0..+3, US for UTC-4..-10), 0 off, 1 EU, 2 US.
 */
static s64 kc_dst_local(time64_t now, int off, time64_t set, int rule)
{
	int std = off, d = 0;

	if (rule < 0)
		rule = off >= 0 && off <= 3 * 3600 ? 1 :
			off >= -10 * 3600 && off <= -4 * 3600 ? 2 : 0;
	if (rule > 2)
		rule = 0;
	if (rule && set > 0 && set <= now) {
		if (rule == 2 && kc_summer(2, set, off))
			std = off - 3600;
		d = kc_summer(rule, now, std) - kc_summer(rule, set, std);
	}
	return now + off + d * 3600;
}

/* "HH:MM" (24 h) or "H:MM"/"HH:MM" (12 h) from the minute of the day. */
static int kc_format_hm(u32 day_min, bool h24, char *s)
{
	u32 hh = day_min / 60, mm = day_min % 60;
	int n = 0;

	if (!h24) {
		hh %= 12;
		if (!hh)
			hh = 12;
	}
	if (h24 || hh >= 10)
		s[n++] = '0' + hh / 10;
	s[n++] = '0' + hh % 10;
	s[n++] = ':';
	s[n++] = '0' + mm / 10;
	s[n++] = '0' + mm % 10;
	return n;
}

/* "Sat 10 Oct" (max 10 chars), S22Cover's DateFormat "EEE d MMM" in en-US:
 * wday 0 = Sunday, mday 1-31, mon 0 = Jan. */
static int kc_format_date(int wday, int mday, int mon, char *s)
{
	static const char days[] = "SunMonTueWedThuFriSat";
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	int i, n = 0;

	if (wday < 0 || wday > 6 || mday < 1 || mday > 31 || mon < 0 ||
	    mon > 11)
		return 0;
	for (i = 0; i < 3; i++)
		s[n++] = days[wday * 3 + i];
	s[n++] = ' ';
	if (mday >= 10)
		s[n++] = '0' + mday / 10;
	s[n++] = '0' + mday % 10;
	s[n++] = ' ';
	for (i = 0; i < 3; i++)
		s[n++] = months[mon * 3 + i];
	return n;
}

/* "85%" or "85% +" while charging or full (S22Cover: status CHARGING or
 * FULL), max 6 chars; cap < 0 (no battery): empty. */
static int kc_format_batt(int cap, bool charging, char *s)
{
	int n = 0;

	if (cap < 0)
		return 0;
	if (cap > 100)
		cap = 100;
	if (cap >= 100)
		s[n++] = '1';
	if (cap >= 10)
		s[n++] = '0' + cap / 10 % 10;
	s[n++] = '0' + cap % 10;
	s[n++] = '%';
	if (charging) {
		s[n++] = ' ';
		s[n++] = '+';
	}
	return n;
}

/* Small font glyph for c (s22_kclock_font.h order), -1 if none. */
static int kc_small_index(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'Z')
		return 10 + c - 'A';
	if (c >= 'a' && c <= 'z')
		return 36 + c - 'a';
	switch (c) {
	case '%':
		return 62;
	case '+':
		return 63;
	case ' ':
		return 64;
	}
	return -1;
}

static inline u16 kc_blend(u16 b, u16 f, u32 a)
{
	u32 na = 255 - a;
	u32 r = ((b >> 11) * na + (f >> 11) * a + 127) / 255;
	u32 g = (((b >> 5) & 63) * na + ((f >> 5) & 63) * a + 127) / 255;
	u32 bl = ((b & 31) * na + (f & 31) * a + 127) / 255;

	return r << 11 | g << 5 | bl;
}

/* Glyph for c in the clock font (digits, ':') or the small font. */
static const struct s22_kclock_glyph *kc_glyph(bool small, char c,
					       const u8 **alpha)
{
	int i;

	if (small) {
		i = kc_small_index(c);
		if (i < 0)
			return NULL;
		*alpha = s22_kclock_small_alpha;
		return &s22_kclock_small_glyphs[i];
	}
	if (c != ':' && (c < '0' || c > '9'))
		return NULL;
	*alpha = s22_kclock_alpha;
	return &s22_kclock_glyphs[c == ':' ? 10 : c - '0'];
}

/* Glyph g (alpha) with its pen at (x, base) blended into pix (pw x ph,
 * clipped) in colour. */
static void kc_blit(u16 *pix, int pw, int ph,
		    const struct s22_kclock_glyph *g, const u8 *alpha, int x,
		    int base, u16 color)
{
	int gx = x + g->bx, gy = base + g->by, px, py;

	alpha += g->off;
	for (py = 0; py < g->h; py++) {
		int ry = gy + py;

		if (ry < 0 || ry >= ph)
			continue;
		for (px = 0; px < g->w; px++) {
			int rx = gx + px;
			u32 al = alpha[py * g->w + px];
			u16 *d;

			if (!al || rx < 0 || rx >= pw)
				continue;
			d = &pix[ry * pw + rx];
			*d = al == 255 ? color : kc_blend(*d, color, al);
		}
	}
}

/* Clock digits centred on cx with the baseline at base, white: the same
 * placement as the client path (kc_render() in s22_kclock.c). */
static void kc_draw_clock(u16 *pix, int pw, int ph, const char *s, int n,
			  int cx, int base)
{
	const struct s22_kclock_glyph *g;
	const u8 *alpha;
	u32 width64 = 0;
	int pen64, i;

	for (i = 0; i < n; i++) {
		g = kc_glyph(false, s[i], &alpha);
		if (g)
			width64 += g->adv64;
	}
	pen64 = cx * 64 - (int)width64 / 2;

	for (i = 0; i < n; i++) {
		g = kc_glyph(false, s[i], &alpha);
		if (!g)
			continue;
		kc_blit(pix, pw, ph, g, alpha, DIV_ROUND_CLOSEST(pen64, 64), base,
			KC_CLOCK_COLOR);
		pen64 += g->adv64;
	}
}

/* Kerning of the pair a b in 1/1024 px (s22_kclock_small_kern). */
static int kc_small_kern(char a, char b)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(s22_kclock_small_kern); i++)
		if (s22_kclock_small_kern[i].a == a &&
		    s22_kclock_small_kern[i].b == b)
			return s22_kclock_small_kern[i].k1024;
	return 0;
}

/* Advance of s[i] in 1/1024 px: hinted advance + kerning with s[i + 1]. */
static int kc_small_adv(const char *s, int n, int i)
{
	const struct s22_kclock_glyph *g;
	const u8 *alpha;
	int a;

	g = kc_glyph(true, s[i], &alpha);
	a = g ? g->adv64 * 16 : 0;
	if (i + 1 < n)
		a += kc_small_kern(s[i], s[i + 1]);
	return a;
}

/*
 * Pen x of each glyph of a small-font line as S22Cover's TextView puts it
 * on a cover xres wide: a view of xres - 2 * S22_KCLOCK_APP_PAD_X at
 * S22_KCLOCK_APP_PAD_X, Layout ALIGN_CENTER (line width truncated and made
 * even, left = (view - width) / 2) and Skia rounding every glyph origin to
 * the nearest pixel (no subpixel positioning).
 */
static void kc_small_pens(const char *s, int n, int xres, int *px)
{
	int run = 0, max, pen, i;

	for (i = 0; i < n; i++)
		run += kc_small_adv(s, n, i);
	max = (run >> 10) & ~1;
	pen = (S22_KCLOCK_APP_PAD_X +
	       ((xres - 2 * S22_KCLOCK_APP_PAD_X - max) >> 1)) << 10;
	for (i = 0; i < n; i++) {
		px[i] = (pen + 512) >> 10;
		pen += kc_small_adv(s, n, i);
	}
}

/* A date or battery line (max 12 chars) with the baseline at base. */
static void kc_draw_small(u16 *pix, int pw, int ph, const char *s, int n,
			  int base)
{
	const struct s22_kclock_glyph *g;
	const u8 *alpha;
	int px[12], i;

	if (n > ARRAY_SIZE(px))
		n = ARRAY_SIZE(px);
	kc_small_pens(s, n, pw, px);
	for (i = 0; i < n; i++) {
		g = kc_glyph(true, s[i], &alpha);
		if (g)
			kc_blit(pix, pw, ph, g, alpha, px[i], base,
				KC_SMALL_COLOR);
	}
}

/*
 * Autonomous screen = S22Cover's screen without its status and notice
 * lines: the five line boxes of its layout (s22_kclock_font.h) stacked and
 * centred like its LinearLayout, the clock centred in the clock box (as
 * the client path) and the date and battery in their boxes.  Only a band
 * of the panel is sent (rows ay..ay+ah-1, full width), centred on the ink;
 * baselines are relative to the band.
 */
#define KC_AUTO_H	64		/* band height (16 KB per redraw) */

struct kc_layout {
	int ay, ah;
	int cx;				/* clock centre */
	int base[3];			/* time, date, battery */
};

static void kc_layout_auto(int xres, int yres, struct kc_layout *l)
{
	int i, y, top, bottom, total = 0, base[3];

	for (i = 0; i < S22_KCLOCK_APP_LINES; i++)
		total += s22_kclock_app_line_h[i];
	y = S22_KCLOCK_APP_PAD_Y + (yres - 2 * S22_KCLOCK_APP_PAD_Y - total) / 2;
	for (i = 0; i < 3; i++) {
		base[i] = y + s22_kclock_app_line_top[i];
		y += s22_kclock_app_line_h[i];
	}
	top = base[0] + s22_kclock_glyphs[0].by;
	bottom = base[2] + S22_KCLOCK_SMALL_DESC;
	l->ah = yres < KC_AUTO_H ? yres : KC_AUTO_H;
	l->ay = top - (l->ah - (bottom - top)) / 2;
	if (l->ay > yres - l->ah)
		l->ay = yres - l->ah;
	if (l->ay < 0)
		l->ay = 0;
	l->cx = S22_KCLOCK_APP_PAD_X + (xres - 2 * S22_KCLOCK_APP_PAD_X) / 2;
	for (i = 0; i < 3; i++)
		l->base[i] = base[i] - l->ay;
}

/* pix (xres x l->ah) = black band with HH:MM, "Sat 10 Oct" and "85%"
 * ("85% +" while charging) for a local minute since the epoch. */
static void kc_render_screen(u16 *pix, int xres, const struct kc_layout *l,
			     s64 minute, bool h24, int cap, bool chg)
{
	char s[12];
	struct tm tm;
	u32 day_min;
	int n;

	memset(pix, 0, xres * l->ah * 2);
	div_u64_rem(minute, 24 * 60, &day_min);
	n = kc_format_hm(day_min, h24, s);
	kc_draw_clock(pix, xres, l->ah, s, n, l->cx, l->base[0]);
	time64_to_tm(minute * 60, 0, &tm);
	n = kc_format_date(tm.tm_wday, tm.tm_mday, tm.tm_mon, s);
	kc_draw_small(pix, xres, l->ah, s, n, l->base[1]);
	n = kc_format_batt(cap, chg, s);
	kc_draw_small(pix, xres, l->ah, s, n, l->base[2]);
}

/*
 * Cover ownership.  "active": the kernel owns the cover (autonomous mode,
 * no cover app): Android's power on/off of fb2 only change its own view,
 * its frames and backlight writes never reach the panel, and the panel
 * follows the lid (closed: on with the clock screen; open: off, light 0).
 * Not active: the panel follows Android (v22c/stock-stable-2 client path).
 *
 * want: the kernel should own it (s22_kclock usable, autonomous=Y, no cover
 * app registered).  busy: Android's real power on/off is running outside
 * kc.lock; nothing changes hands until it ends (it requeues the work).
 */
static bool kc_want(bool usable, bool autonomous, bool client)
{
	return usable && autonomous && !client;
}

#define KC_P_ENTER	0x01	/* take ownership: Android becomes virtual */
#define KC_P_LEAVE	0x02	/* hand back: Android on -> full power on, its
				 * last frame and its light; off -> panel off */
#define KC_P_RELEASE	0x04	/* lid open: panel off, light 0 */
#define KC_P_TAKE	0x08	/* lid closed: panel on (if off), clock screen */

/* What the work does now, applied in bit order (ENTER before TAKE). */
static int kc_plan(bool want, bool busy, bool active, bool lid_open,
		   bool own, bool powered)
{
	int plan = 0;

	if (busy)
		return 0;
	if (!want)
		return active ? KC_P_LEAVE : 0;
	if (!active)
		plan |= KC_P_ENTER;
	if (!lid_open) {
		if (!own)
			plan |= KC_P_TAKE;
	} else if (own || powered) {
		plan |= KC_P_RELEASE;
	}
	return plan;
}

#endif

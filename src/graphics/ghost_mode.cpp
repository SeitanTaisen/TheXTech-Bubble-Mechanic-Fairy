#include "ghost_mode.h"

#include "../globals.h"    // numPlayers, etc.
#include "../player.h"     // Player[], Player_t
#include "../collision.h"  // CheckCollision(..) if you want engine's AABB
#include "../effect.h"     // (optional) NewEffect(...)
#include "../sound.h"      // (optional) PlaySound(...)
#include "../screen.h"     // vScreenByPlayer(...)
#include "../eff_id.h"   // EFFID_* enums

// Remember previous viewport positions to detect camera motion
static int s_prevLeft[256] = {0};
static int s_prevTop [256] = {0};

// How much faster we count OOB when camera moves away and you're trailing
static const int FAST_STEP_MULT = 12; // 6x faster => ~5 frames with kOOBFrames=30

// ================== TUNABLES ==================
static const int   kOOBPadX        = 6;   // was 24 (helps when moving RIGHT/LEFT)
static const int   kOOBPadY        = 6;   // was 24 (helps when moving UP/DOWN)
static const int   kOOBFrames      = 16;  // was 30 (shorter grace)
static const int   kReviveIFrames  = 30;  // keep
static const int   kGhostSpawnIFrm = 50;  // keep
// ==============================================

// Internal state (separate from Player_t)
static bool s_isGhost[256]  = { false };
static int  s_oobFrames[256]= { 0 };
// --- Grace period after revive to prevent instant re-ghost ---
static int  s_reviveGrace[256] = { 0 };
static const int kReviveGraceFrames = 20; // ~0.33s at 60 FPS
// --- Minimum time a new ghost must stay ghosted (prevents instant revive) ---
static int  s_ghostMinFrames[256] = { 0 };
static const int kGhostMinFrames = 30; // ~0.5s at 60 FPS
static bool s_prevFairy[256] = { false };
static int  s_prevFairyTime[256] = { 0 };
// How much to lift a player when reviving to avoid ground/wall embedding
static const int kRevivePopUpPx = 8;   // vertical pixels
static const int kReviveHopVy   = 3;   // initial upward speed (px/frame)
static int s_postReviveNoSnap[256] = {0};
static const int kPostReviveNoSnapFrames = 2;
// --- All-ghosts failsafe -----------------------------------------------------
// How long (in frames) to wait before auto-reviving everyone if *all* players are ghosts.
// 180 ~= 3 seconds at 60 FPS.
static const int kAllGhostAutoReviveFrames = 180;
static int s_allGhostFrames = 0;
// --- Cooldown: block self-ghost re-trigger after a revive ---
static int s_selfGhostCooldown[256] = {0};
static const int kSelfGhostCooldownFrames = 120; // ~2 seconds at 60 FPS

static inline void Ghost_AutoReviveAll_Tick()
{
    if(numPlayers <= 0) { s_allGhostFrames = 0; return; }

    int ghosts = 0;
    for(int i = 1; i <= numPlayers; ++i)
        if(Ghost_IsGhost(i))
            ++ghosts;

    if(ghosts > 0 && ghosts == numPlayers)
    {
        // Everyone is a ghost right now — start/continue the timer.
        if(++s_allGhostFrames >= kAllGhostAutoReviveFrames)
        {
            // Time's up: bring everyone back.
            for(int i = 1; i <= numPlayers; ++i)
            {
                if(Ghost_IsGhost(i))
                {
                    // toucher=0 => revive in-place (no toucher needed)
                    Ghost_ReviveFromGhost(i, 0);
                }
            }
            s_allGhostFrames = 0;
        }
    }
    else
    {
        // Not all-ghosts anymore — reset the timer.
        s_allGhostFrames = 0;
    }
}
// ---- Ghost feature gate -----------------------------------------------------
static inline bool Ghost_FeatureEnabled()
{
    // Disable ALL ghost mechanics when exactly two players are active
    // (keeps standard 1P revival, 3–5P ghosting, etc. untouched)
    return (numPlayers != 2);
}
// Small local AABB for players (to avoid extra includes if you prefer)
static inline bool aabbOverlap(const Location_t &a, const Location_t &b)
{
    return !(a.X + a.Width  < b.X || b.X + b.Width  < a.X ||
             a.Y + a.Height < b.Y || b.Y + b.Height < a.Y);
}

bool Ghost_IsGhost(int id)
{
    if(id < 1 || id > numPlayers) return false;
    return s_isGhost[id];
}

void Ghost_MakeGhost(int id)
{
    if(id < 1 || id > numPlayers) return;
    if(s_isGhost[id]) return;
    Player_t &p = Player[id];
    if(p.Dead) return;

    s_isGhost[id] = true;
    // remember prior fairy state, then force fairy-ghost
	s_prevFairy[id] = Player[id].Fairy;
	s_prevFairyTime[id] = Player[id].FairyTime;
	Player[id].Fairy = true;
	Player[id].FairyTime = -1;
	s_ghostMinFrames[id] = kGhostMinFrames;

    // brief safety + stop drift
    if(p.Immune < kGhostSpawnIFrm) p.Immune = kGhostSpawnIFrm;
    p.Location.SpeedX = 0;
    p.Location.SpeedY = 0;
	p.Effect = PLREFF_NO_COLLIDE; // <- actually become intangible

    // (optional) visuals/sfx
     PlaySound(SFX_HeroFairy);
    NewEffect(EFFID_SMOKE_S3_CENTER, p.Location);
}

void Ghost_ReviveFromGhost(int id, int toucher)
{
    if(id < 1 || id > numPlayers) return;
    if(!s_isGhost[id]) return;

    s_postReviveNoSnap[id] = kPostReviveNoSnapFrames;

    Player_t &p = Player[id];
    s_isGhost[id] = false;

    // --- Place the revived player safely and give a tiny hop ---
    if(toucher > 0 && toucher <= numPlayers)
    {
        const Player_t &t = Player[toucher];

        const num_t tMidX = t.Location.X + (t.Location.Width / 2);
        const num_t newX  = tMidX - (p.Location.Width / 2);
        const num_t newY  = t.Location.Y - p.Location.Height - num_t(kRevivePopUpPx);

        p.Location.X = newX;
        p.Location.Y = newY;
    }
    else
    {
        // No toucher: pop up in place a little to escape floor/walls
        p.Location.Y -= num_t(kRevivePopUpPx);
    }

    // Small upward impulse and reset horizontal drift
    p.Location.SpeedY = -num_t(kReviveHopVy);
    p.Location.SpeedX = 0_n;

    // Ensure the engine won’t think we’re “already standing” this frame
    p.Slope   = 0;
    p.StandUp = true;     // <— helps collision recalc with full height

    // restore fairy flags/state
    p.Fairy     = s_prevFairy[id];
    p.FairyTime = s_prevFairyTime[id];
    p.Effect    = PLREFF_NORMAL;  // collide again as a normal player

    s_reviveGrace[id] = kReviveGraceFrames;

    if(p.Immune < kReviveIFrames)
        p.Immune = kReviveIFrames;

    PlaySound(SFX_Transform);
	s_selfGhostCooldown[id] = kSelfGhostCooldownFrames;
}
void Ghost_KeepStateAlive(int id)
{
    if(id < 1 || id > numPlayers) return;
    if(!s_isGhost[id]) return;

    Player_t &p = Player[id];

    // Make sure we stay intangible and “fairy”-rendered while ghosted
    p.Effect    = PLREFF_NO_COLLIDE;
    p.Fairy     = true;
    p.FairyTime = -1;

    // Keep a small immunity floor while ghosted (prevents instant damage flicker)
    if(p.Immune < kGhostSpawnIFrm / 2)
        p.Immune = kGhostSpawnIFrm / 2;
    // NEW: keep physics neutral so nothing fights our drift
    p.Location.SpeedX = 0;
    p.Location.SpeedY = 0;
	// ↓↓↓ NEW: tick down the “must-stay-ghost” window
    if(s_ghostMinFrames[id] > 0)
        --s_ghostMinFrames[id];
}
void Ghost_CheckOutOfBounds()
{
    if(!Ghost_FeatureEnabled()) return;  // <-- add this line
    // Soft guard against immediate ground/slope snap the first 1–2 frames after revive
	for(int i = 1; i <= numPlayers; ++i)
    {
        if(s_postReviveNoSnap[i] > 0)
        {
            Player_t &pp = Player[i];
            // No "Standing" field in Player_t; clear slope to avoid first-frame snap
            pp.Slope = 0;
            --s_postReviveNoSnap[i];
		}
	}
    Ghost_AutoReviveAll_Tick(); // <--- add this
    for(int id = 1; id <= numPlayers; ++id)
    {
        Player_t &p = Player[id];
		if(s_reviveGrace[id] > 0)
		{
			s_reviveGrace[id]--;   // count down the grace frames
			s_oobFrames[id] = 0;   // don't accumulate OOB frames during grace
			continue;              // skip OOB checks this frame
		}
        if(p.Dead)        { s_oobFrames[id] = 0; continue; }
        if(s_isGhost[id]) { s_oobFrames[id] = 0; continue; }

        // Use the real viewport for this player
        vScreen_t &vs = vScreenByPlayer(id); // from screen.h
        if(!vs.Visible)   { s_oobFrames[id] = 0; continue; }

        // Player center against the expanded view (your current logic)
        const int px = (int)p.Location.X;
        const int py = (int)p.Location.Y;
        const int pw = (int)p.Location.Width;
        const int ph = (int)p.Location.Height;

        const int cx = px + (pw / 2);
        const int cy = py + (ph / 2);

        const int L = vs.Left                - kOOBPadX;
        const int R = vs.Left + vs.Width     + kOOBPadX;
        const int T = vs.Top                 - kOOBPadY;
        const int B = vs.Top  + vs.Height    + kOOBPadY;

        const bool inside = (cx >= L && cx <= R && cy >= T && cy <= B);

        // --- NEW: detect camera motion & trailing status
        const int dxCam = vs.Left - s_prevLeft[id];
        const int dyCam = vs.Top  - s_prevTop [id];

        const bool movingRight = (dxCam > 0);
        const bool movingLeft  = (dxCam < 0);
        const bool movingDown  = (dyCam > 0);
        const bool movingUp    = (dyCam < 0);

        // "Trailing" means you’re falling behind opposite to camera motion.
        // Example: camera moving right and your center is beyond the LEFT edge (with pad).
        const bool trailing =
            (movingRight && (cx < L)) ||
            (movingLeft  && (cx > R)) ||
            (movingDown  && (cy < T)) ||
            (movingUp    && (cy > B));

        // Step size for OOB counter: faster when trailing while camera moves.
        int step = 1;
        if((movingRight || movingLeft || movingUp || movingDown) && trailing)
        {
            step = FAST_STEP_MULT;

            // If you ever need to EXCLUDE AUTOSCROLLING levels explicitly,
            // gate this with your autoscroll flag, e.g.:
            // if(g_isAutoscrolling) step = 1;
        }

        if(inside)
        {
            s_oobFrames[id] = 0;
        }
        else
        {
            s_oobFrames[id] += step;
            if(s_oobFrames[id] >= kOOBFrames)
            {
                Ghost_MakeGhost(id);
                s_oobFrames[id] = 0;
            }
        }

        // Remember camera for next frame
        s_prevLeft[id] = vs.Left;
        s_prevTop [id] = vs.Top;
    }
}
// ================== SELF-GHOST BY 4-BUTTON CHORD ==================
// NOTE: This branch doesn't expose per-button "Held" helpers.
// To keep builds green, we stub the chord as "never held" for now.
// (Out-of-bounds ghosting still works as before.)

// 4-button chord: Run + AltRun + Jump + AltJump
// Returns true only when ALL FOUR are held for player `id` (1-based).
static bool isChordHeld(int id)
{
    if(id < 1 || id > numPlayers) return false;

    const Player_t &p = Player[id];
    // Require RUN + ALT RUN + JUMP + ALT JUMP all held together
    return (p.Controls.Run &&
            p.Controls.AltRun &&
            p.Controls.Jump &&
            p.Controls.AltJump);
}


static bool s_prevChordHeld[256] = {false};

bool Ghost_TrySelfGhostByButtons(int id)
{
	if (s_selfGhostCooldown[id] > 0) return false;
    if(id < 1 || id > numPlayers) return false;
	if(s_selfGhostCooldown[id] > 0) return false;
    // Only allow when there are 2+ living players,
    // and prevent the last remaining living player from ghosting.
    int living = 0;
    for(int i = 1; i <= numPlayers; ++i)
        if(!Player[i].Dead && !Ghost_IsGhost(i))
            ++living;
    if(living <= 1) return false;

    const bool held = isChordHeld(id);
    bool fired = false;

    // edge trigger: only on press, not while held
    if(held && !s_prevChordHeld[id])
    {
        Ghost_MakeGhost(id);
        fired = true;
    }

    s_prevChordHeld[id] = held;
    return fired;
}

void Ghost_SelfGhostByButtons()
{
    if(!Ghost_FeatureEnabled()) return;
	for (int i = 1; i <= numPlayers; ++i)
    {
		if (s_selfGhostCooldown[i] > 0)
			--s_selfGhostCooldown[i];
	}
    // Count how many players are alive and not ghosts
    int alive_not_ghost = 0;
    for(int i = 1; i <= numPlayers; ++i)
        if(!Player[i].Dead && !s_isGhost[i])
            ++alive_not_ghost;

    // Walk all players and allow self-ghost on a *rising edge* of the chord
    for(int id = 1; id <= numPlayers; ++id)
    {
        if(Player[id].Dead)        { s_prevChordHeld[id] = false; continue; }
        if(s_isGhost[id])          { s_prevChordHeld[id] = false; continue; }

        const bool held = isChordHeld(id);

		if (s_selfGhostCooldown[id] > 0) {
			s_prevChordHeld[id] = held; // keep edge-tracking consistent
			continue;
		}
        // Only trigger once (edge), and only when at least two alive non-ghost players exist
        if(held && !s_prevChordHeld[id])
        {
            // If this player is the last alive, non-ghost -> block the request
            const bool is_last_standing = (alive_not_ghost <= 1);
            if(!is_last_standing)
            {
                Ghost_MakeGhost(id);
                // After this call, they become a ghost; no need to adjust alive_not_ghost this frame.
            }
            // else: silently ignore (can e.g. PlaySound(SFX_BlockHit) if you want feedback)
        }

        s_prevChordHeld[id] = held;
    }
}

bool Ghost_TrySelfGhost(int id)
{
    if(!Ghost_FeatureEnabled()) return false;
    if(id < 1 || id > numPlayers) return false;
    if(Player[id].Dead)           return false;

    // If already a ghost, only allow manual revive after min window
    if(Ghost_IsGhost(id))
    {
        if(s_ghostMinFrames[id] > 0)  // NEW: honor lockout
            return false;
        Ghost_ReviveFromGhost(id, 0);
        return true;
    }

    // Count alive, non-ghost players (to protect the last one)
    int alive_not_ghost = 0;
    for(int i = 1; i <= numPlayers; ++i)
        if(!Player[i].Dead && !Ghost_IsGhost(i))
            ++alive_not_ghost;

    if(alive_not_ghost <= 1)
        return false;

    Ghost_MakeGhost(id);
    return true;
}

// ================= END SELF-GHOST CHORD =================

void Ghost_TouchRevive()
{
    if(!Ghost_FeatureEnabled()) return;
	// --- Failsafe: if literally everyone is ghosted, auto-revive all after a delay
    Ghost_AutoReviveAll_Tick();
    for(int g = 1; g <= numPlayers; ++g)
    {
        if(!s_isGhost[g]) continue;
        if(Player[g].Dead) continue;

        // NEW: enforce ghost state every frame so it can't be cleared by other systems
        Ghost_KeepStateAlive(g);
		auto findNearestAlive = [](int self)->int {
			int best = 0;
			num_t bestD = 0_n;
			const Location_t &me = Player[self].Location;
			const num_t mcx = me.X + me.Width / 2;
			const num_t mcy = me.Y + me.Height / 2;

			for(int i = 1; i <= numPlayers; ++i) {
				if(i == self)                continue;
				if(Player[i].Dead)           continue;
				if(Ghost_IsGhost(i))         continue;
				const Location_t &o = Player[i].Location;
				const num_t ocx = o.X + o.Width / 2;
				const num_t ocy = o.Y + o.Height / 2;
				const num_t dx = ocx - mcx;
				const num_t dy = ocy - mcy;
				const num_t d  = num_t::abs(dx) + num_t::abs(dy); // cheap L1 distance
				if(best == 0 || d < bestD) { best = i; bestD = d; }
			}
			return best;
		};

		{
			Player_t &pg = Player[g];
			const int t = findNearestAlive(g);
			if(t > 0) {
				const Location_t &me = pg.Location;
				const Location_t &to = Player[t].Location;

				const num_t mcx = me.X + me.Width  / 2;
				const num_t mcy = me.Y + me.Height / 2;
				const num_t tcx = to.X + to.Width  / 2;
				const num_t tcy = to.Y + to.Height / 2;

				num_t dx = tcx - mcx;
				num_t dy = tcy - mcy;

				const num_t len = num_t::abs(dx) + num_t::abs(dy);
				if(len > 0)
				{
					const num_t nx = dx.divided_by(len);
					const num_t ny = dy.divided_by(len);

					// Base ghost follow speed (tweak to taste)
					int kStep = 3;

					// Hold RUN/ALT-RUN to move faster
					if(pg.Controls.Run || pg.Controls.AltRun)
						kStep = 7;  // bump this if you want it snappier

					// Glide the ghost toward the target
					pg.Location.X += kStep * nx;
					pg.Location.Y += kStep * ny;

					// optional: small cosmetic bob or effect could be added here
					

					// keep velocities neutral so engine physics doesn't fight this
					pg.Location.SpeedX = 0;
					pg.Location.SpeedY = 0;
				}
		}
        const Location_t &gbox = Player[g].Location;

        if(s_ghostMinFrames[g] > 0)
        {
            s_ghostMinFrames[g]--;
            continue; // can't revive yet, but stay ghosted
        }
        for(int a = 1; a <= numPlayers; ++a)
        {
            if(a == g) continue;
            if(s_isGhost[a]) continue;
            if(Player[a].Dead) continue;
            if(Player[a].Section != Player[g].Section) continue;

            const Location_t &abox = Player[a].Location;

            // Use engine AABB if you prefer:
            // if(CheckCollision(abox, gbox)) { ... }
            if(aabbOverlap(abox, gbox))
            {
                Ghost_ReviveFromGhost(g, a);
                break;
            }
        }
		}
    }
}
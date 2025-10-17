#pragma once

// Simple drop-in ghost system for local multiplayer

// Call once per frame *after* you update / center cameras.
void Ghost_CheckOutOfBounds();

// Call once per frame *after* physics & collisions are resolved.
void Ghost_TouchRevive();

// Query in other places (e.g., camera code) if you want to ignore ghosts.
bool Ghost_IsGhost(int id);

// Force-set (rarely needed; provided for future net-play sync or scripted events)
void Ghost_MakeGhost(int id);
void Ghost_ReviveFromGhost(int id, int toucher /* or 0 if none */);

// Let a player voluntarily become a ghost by holding a 4-button chord.
// Safety: requires at least 2 alive, non-ghost players; the last one can't ghost.
void Ghost_SelfGhostByButtons();
bool Ghost_TrySelfGhost(int id);  // for cheats/menus to trigger self-ghost
// NEW: keep the ghost state enforced every frame
void Ghost_KeepStateAlive(int id);
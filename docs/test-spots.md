# In-game test spots

Start the game at a checkpoint instead of playing to it:

```
tools/run_game_logged.sh subway            # a named spot (table below; "spots" lists them)
tools/run_game_logged.sh Mall_p:Mall       # any map and checkpoint
```

MirrorsEdge.exe ignores maps on its command line: at start-up it copies a fixed string over the command line
(0x010eb38a: the flyby demo URL with `-FlybyFlight`, `-nostartupmovies` with `-nostartupmovies`, otherwise empty)
[V: disassembly; also runs 20261012-161126/161206 landed in the main menu]. The engine then starts `[URL] LocalMap` from
TdEngine.ini, so the script writes the spot there in the clone (`Map_p?LoadCheckpoint=Name?Game=TdGame.TdSpStoryGame`,
the form of the exe's own start list) and puts `TdMainMenu` back on any launch without a spot [V: runs
20261012-161856 (escape) and 164047 (subway) created their cloths before the first simulate; a menu start needed ~9000]. A key binding to
`start <url>` (F11) did nothing at the main menu. Our fluids and cloths run by default; logs land in
`captures/game-run/<timestamp>/` and the script prints the last coverage line at exit.

## Spots

Nearest checkpoint (straight line) to placed PhysX content. Some effects start on a scripted event, so walk ahead a bit.

| spot | map:checkpoint | what is there | distance |
|---|---|---|---|
| subway | Subway_p:Renovation_Fight | tearable plastic sheet with bEnableValidBounds (row 4) | 36 m |
| escape | Escape_p:Chase_SolarPanels | vent heli smoke (SPH, the ~6500-particle case); cloths get valid bounds at run time | 41 m |
| mall | Mall_p:After_fan | tearable plastic sheets | 40 m |
| cranes | Cranes_p:SP03_Afterbalance | crane cloths with bEnableValidBounds | 27 m |
| scraper | Scraper_p:Out_soft | explosion smoke | 7 m |
| factory | Factory_p:Conveyor_puzzle | coolant smoke | 32 m |
| stormdrain | Stormdrain_p:Waterfall | waterslide smoke, waterfall | 23 m |
| boat | Boat_p:Celeste_Chase | steam | 50 m |
| edge | Edge_p:Cops | shootout: glass and debris | |

## Checkpoint names per story map

Read from TdCheckpoint.CheckpointName in the `*_p.me1` packages (exact spelling).

- Tutorial_p: start
- Edge_p: Edge_Start, After_Intro, Rooftop_Action, Before_Balance_Walk, After_Balance_Walk, Cops, SWAT_Response, SWAT_Response2, News_Chopper
- Escape_p: Start, after_intro_flight, Office, Office_AfterCS, RPA_Lower, After_Office, Chase, ChaseFlyby, Chase_SoftLanding, Chase_SolarPanels, Elev_R1-St1, Avenue, Plaza, 01bPlaza_Tunnels
- Stormdrain_p: JKfight, Canals, Canals_checkpoint_1..3, Container, Container_cp1..3, Gate_3..9, Gate1_cp4..6, Chute2, Waterfall, Sluice_canals1, Sluice_out1, Sluice_Out_RoofUp, Exit_checkpoint1..3, after_loading_boss, Boss_checkpoint1, jk_lastjump, ChaseJK, PillarRoom_Savepo... (37 in all)
- Cranes_p: SP03_Start, SP03_Rooftop_01, SP03_Rooftop_02, SP03_Rooftop_before_swats, SP03_Office_01, SP03_Office_02, SP03_Office_03, SP03_Office_03_slc, SP03_half_puzzle, First_sluice, CP_sluiceRoofPlaza, SP03_Plaza_01..04, SP03_Afterbalance
- Subway_p: Start_Point_subway, Start_subway_after, Subway_Station, Platform_fight, Subway_Tunnels, subway_traintunnel, Train_Ride, Train_Ride_End, Renovation, subway_inside_renovation, Renovation_Fight, ren-renco_slc, renco_after_cs, FanP, scaffolding_stat, after_scaffolding, Subway_bossfight, boss_fight_no_cs
- Mall_p: LevelStart, LevelAfter_cutscene, Mall_outside, Mall, Mall_Ambush, After_Ambush, After_fan, After_puzzle, Blue_Vent, CombatPuzzle1_Finished, CombatRooftops, CP_afterSWAT, R1_AfterCombat_Checkpoint, R1_FinalCombat_Checkpoint, R2_tomall, R2beforevent, R2_Stretch_Checkpoint, Stretch, Sluice_Hw, LastCP
- Factory_p: Start_point, Training_area, Training_area_after_cs, Loading_bay, Loading_bay_after, Loading_bay_2, Loading_bay_2b, LoadingBay_LowerDoor, Conveyor_puzzle, steamroom_puzzle, factory_renovafterCS, factory_sniper, factory_roof, after_facto, TO_arena, Pursuit_chase, Pursuit_chase_middle, Pursuit_chase_end
- Boat_p: Start, Vent_Start, Ventilation, Ventilation_2, Ventilation_3, CarDeck, CarDeck_Upper, TopBoat, Approaching_Sniper, Boat_Sniper_Top, Boat_Sniper_Interior, Celeste, Celeste_Chase, Celeste_Chase2, Truck, Boat_Truckride
- Convoy_p: After_intro08, Start_chasing, LandingAfterChase, Elevator_to_Conv, kates_convoy, Stretch2, CP_Vent, Atrium, Atrium2, atrium_3, Atrium_soft_cp, 1st_floor, 2nd_floor, 3rd_floor, 4th_floor, combat1, combat_2, combat_3, Sniper
- Scraper_p: Scraper_Start, scraper_before_lobby, Lobby, Scraper_Inside_elevator_lobby, Elevator_shaft, Elevator_shaft_b, Duct_streaming, Server_room, Scraper_Mill-Main_Slc, subway_roof-mill, Carpark, Out_soft, Rooftops, scraper_goldenswing, End_game

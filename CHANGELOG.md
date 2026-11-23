# fteqw-sv changelog

## nextbot-1.1

Nav meshes are now version 9; older `.qnav` files are regenerated automatically the first time
each map is played.

### Bots: movement and navigation
* Running jumps: the generator finds jumps across gaps that need a run-up (dm4ish's quad
  balcony, ledges on dm2, dm3, ztndm3, aerowalk) and bots time the jump and steer in the air.
* Ramps and ledges: paths only cross between nav areas where the step is climbable. Bots no
  longer walk into the side of a ramp or stop under a ledge (2fort5r's battlement ramps).
* Edge perches: standing spots held up by a sliver of the player box are no longer generated.
* Stuck detection counts a goal straight above or below the bot (it used to think it had
  arrived), and stuck bots sidestep before re-pathing.
* Lifts: bots wait at the edge of a `func_plat` shaft until the lift is down (waiting in the
  shaft holds the lift up forever; standing under it gets you crushed), and ride objectives on
  top of lifts.
* Water: drops into deep water are followed to the bottom, and bots can swim up and climb out.
* Traps: areas with no way back to a spawn are flagged. Bots avoid them and respawn after 5 s if
  they fall in (4f4's flooded hall).
* `qw_bot_teamghost` (default 1): bots pass through teammate and allied bots instead of jamming
  narrow corridors (Hunted). Humans still collide with everyone.
* The no-score respawn (`qw_bot_stucktime`) only applies to bots that are also not moving around.

### Bots: Team Fortress
* Fights: bots only stop to fight within an engage range (snipers further), give up stand-offs
  that aren't closing (balcony to balcony on 2fort5r) and shoot on the move otherwise. Fights
  close distance along the nav mesh.
* Flag runs: a trip continues while it makes progress (along the path, not just in a straight
  line), and its clock stops during fights. Attackers spread over the available flags. Reverse
  items (`goal_activation` 64: any team but the owner may take it) are stolen.
* Hunted: the President rides the escape pillar and scores.
* Spies (TF 2.8): disguise keys are only pressed when the menu is really open (a stray key could
  pick "Start Feigning"); a bot that ends up feigning stands back up.
* `qw_tf_allies` (default 1, TF 2.8/2.9 progs): allied teams can't hurt each other; medics heal
  and engineers repair allies. Done by wrapping `T_Damage`, `TF_T_Damage`, `W_FireMedikit` and
  `W_FireSpanner` from the engine.

### Server
* `qw_autojump`: alias of `pm_autobunny` (hold jump to keep jumping).
* `sv_heatmap`: optional position/death/movement sampler for heatmap sites (off by default).
* dpmaster `getinfo` replies fixed (the server info and challenge were missing), so dpmaster
  masters and FortressOne's server browser list the server again. `com_protocolname` can list
  several protocol names; the first is advertised and all may connect.

## nextbot-1.0

First release: NextBot-style bots for QuakeWorld deathmatch and Team Fortress (FortressOne and
TF 2.8/2.9), generated nav meshes, mapcycle and intermission handling.

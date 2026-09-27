#pragma once

// Other worlds' items drawn as game boxes: a cover on the front and back, edges in a colour from
// the art. The box takes the place of the item's own model (the Sol) in the item's draw, sized
// from that model and moving with it, so it spins on the ground and rises in Link's hands the
// way the Sol would.

class daItemBase_c;

namespace ap::covers {
struct Cover;
}

namespace ap::box {

// Draws `item` as a box with this cover instead of its model. False if it can't (no model).
// `held`: Link is holding it up, so it stands straight; in the world it leans back a little and
// sits smaller, like a box on display as it turns.
bool draw(daItemBase_c* item, const covers::Cover& cover, bool held);

// Once per game tick: forgets boxes whose items are gone.
void tick();

}  // namespace ap::box

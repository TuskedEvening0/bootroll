#pragma once
// BCD professional (element) mode (M2): browse and edit every element of any
// object — smart editors per registry type plus raw hex editing, and
// add/remove of arbitrary element IDs.
namespace bootroll {

class App;

struct BcdProfessionalScreen {
    static void drawBody(App& app);
};

} // namespace bootroll

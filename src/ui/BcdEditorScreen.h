#pragma once
// BCD page (M1/M2): open/save a BCD store. Two sub-tabs:
//   Easy         — boot manager globals + osloader entry list (M1)
//   Professional — element-level browse/edit of any object (M2)
namespace bootroll {

class App;

struct BcdEditorScreen {
    static void drawBody(App& app);
    static void drawEasyMode(App& app); // body of the "Easy" sub-tab
};

} // namespace bootroll

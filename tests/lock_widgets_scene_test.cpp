#include "lock_widgets_scene.h"
#include "render/scene/rect_node.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "tests/test_check.h"
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
class MeasurementRenderer final : public Renderer {
public:
  TextMetrics measureText(std::string_view text, float size, FontWeight, float maxWidth, int,
      TextAlign, std::string_view, TextEllipsize, bool) override {
    const float width = static_cast<float>(text.size()) * size * .5F;
    return {.width = maxWidth > 0 ? std::min(width,maxWidth) : width, .right = width, .bottom = size, .lineCount = 1};
  }
  TextMetrics measureFont(float size, FontWeight) override { return {.bottom = size}; }
  void measureTextCursorStops(std::string_view, float size, const std::vector<std::size_t>& offsets,
      std::vector<float>& result, FontWeight) override {
    result.clear(); for (auto offset : offsets) result.push_back(static_cast<float>(offset) * size * .5F);
  }
  void measureTextCursorStopsWrapped(std::string_view, float, const std::vector<std::size_t>&,
      float, std::vector<TextCursorStop>&, FontWeight) override {}
  TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .right = size, .bottom = size}; }
  TextureManager& textureManager() override { std::abort(); }
  float renderScale() const noexcept override { return 1; }
};

bool near(float a,float b) { return std::abs(a-b)<.01F; }
const RectNode* background(const Node* node) {
  if (node->type()==NodeType::Rect) return static_cast<const RectNode*>(node);
  for (const auto& child:node->children()) if (const auto* found=background(child.get())) return found;
  return nullptr;
}
bool containsText(const Node* node,std::string_view text) {
  if (const auto* label=dynamic_cast<const Label*>(node);label && label->text()==text) return true;
  for (const auto& child:node->children()) if (containsText(child.get(),text)) return true;
  return false;
}
bool belongsTo(const Node* node,const Node* ancestor) {
  for (;node;node=node->parent()) if(node==ancestor) return true;
  return false;
}
}

int main() {
  MeasurementRenderer renderer;
  Node root;root.setFrameSize(800,600);
  auto auth=std::make_unique<Node>();auth->setPosition(10,10);auth->setFrameSize(300,100);
  auto input=std::make_unique<Input>();auto* field=input.get();
  input->setValue("retained authentication input");input->setFrameSize(250,40);
  auth->addChild(std::move(input));auto* authNode=root.addChild(std::move(auth));
  const auto retainedAuth=[&] {
    TEST_CHECK(authNode->parent()==&root && field->parent()==authNode);
    TEST_CHECK(field->value()=="retained authentication input");
  };
  {
    LockWidgetsScene scene(root);
    greeter_appearance::LockWidget label;
    label.id="decoration";label.type="label";label.cx=130;label.cy=60;
    label.boxWidth=240;label.boxHeight=100;
    label.settings={{"title",std::string("Public greeting")},{"description",std::string{}},
        {"background",true},{"background_opacity",.35},{"background_radius",18.0},
        {"background_padding",12.0}};
    greeter_appearance::LockWidgetLayout layout{{label}};
    // Default-output widgets appear only on the primary view.
    scene.sync(layout,"DP-2",false,renderer,800,600);
    TEST_CHECK(root.children().size()==1);retainedAuth();
    scene.sync(layout,"DP-1",true,renderer,800,600);
    TEST_CHECK(root.children().size()==2 && !scene.hasClock());
    const auto* decoration=root.children().back().get();
    TEST_CHECK(containsText(decoration,"Public greeting"));
    TEST_CHECK(near(decoration->x(),10) && near(decoration->y(),10));
    TEST_CHECK(near(decoration->width(),240) && near(decoration->height(),100));
    const auto* bg=background(decoration);
    TEST_CHECK(bg && near(bg->style().fill.a,.35F) && near(bg->style().radius.tl,18));
    TEST_CHECK(bg->materialBackdropLocal());
    // The decoration overlaps auth, yet neither it nor its descendants intercept input.
    TEST_CHECK(!decoration->hitTestVisible());
    TEST_CHECK(belongsTo(Node::hitTest(&root,20,20),authNode));
    scene.sync(layout,"DP-1",true,renderer,800,600);
    TEST_CHECK(root.children().back().get()==decoration);retainedAuth();
    // A named output removes the old view and restores the requested replica.
    layout.widgets[0].output="DP-2";
    scene.sync(layout,"DP-1",true,renderer,800,600);
    TEST_CHECK(root.children().size()==1);
    scene.sync(layout,"DP-2",false,renderer,800,600);
    TEST_CHECK(root.children().size()==2);retainedAuth();
    // Replacing the same decoration ID changes its real widget subtree, not auth.
    layout.widgets[0].type="clock";layout.widgets[0].settings.clear();
    scene.sync(layout,"DP-2",false,renderer,800,600);
    TEST_CHECK(scene.hasClock() && root.children().size()==2);retainedAuth();
    layout.widgets[0]=label;layout.widgets[0].settings["title"]=std::string("Replacement greeting");
    scene.sync(layout,"DP-1",true,renderer,800,600);
    TEST_CHECK(!scene.hasClock());
    TEST_CHECK(containsText(root.children().back().get(),"Replacement greeting"));retainedAuth();
    layout.widgets[0].enabled=false;
    scene.sync(layout,"DP-1",true,renderer,800,600);
    TEST_CHECK(root.children().size()==1);retainedAuth();
    layout.widgets[0].enabled=true;
    scene.sync(layout,"DP-1",true,renderer,800,600);
    // Reordering unchanged descriptors must change actual retained paint order.
    auto second = label; second.id = "second";
    second.settings["title"] = std::string("Second decoration");
    layout.widgets = {label, second};
    scene.sync(layout,"DP-1",true,renderer,800,600);
    auto* firstNode = root.children()[1].get();
    auto* secondNode = root.children()[2].get();
    layout.widgets = {second, label};
    scene.sync(layout,"DP-1",true,renderer,800,600);
    TEST_CHECK(root.children()[1].get()==secondNode && root.children()[2].get()==firstNode);
    retainedAuth();
    layout.widgets = {label};
    scene.sync(layout,"DP-1",true,renderer,800,600);
    const auto* retainedGreeting = root.children().back().get();
    for (const auto* type : {"volume", "sysmon", "audio_visualizer", "fancy_audio_visualizer"}) {
      auto serviceWidget = label;
      serviceWidget.id = "service"; serviceWidget.type = type;
      serviceWidget.settings.clear();
      serviceWidget.settings["show_when_idle"] = true;
      layout.widgets.push_back(serviceWidget);
      scene.sync(layout,"DP-1",true,renderer,800,600);
      TEST_CHECK(root.children().size()==3);
      TEST_CHECK(root.children()[1].get()==retainedGreeting);
      TEST_CHECK(!root.children().back()->hitTestVisible());
      TEST_CHECK(belongsTo(Node::hitTest(&root,20,20),authNode));
      if (serviceWidget.type != "audio_visualizer")
        TEST_CHECK(containsText(root.children().back().get(),"--"));
      TEST_CHECK(!scene.needsFrameTick());
      scene.frameTick(16,renderer); retainedAuth();
      layout.widgets.pop_back();
      scene.sync(layout,"DP-1",true,renderer,800,600);
      TEST_CHECK(root.children().size()==2 && !scene.needsFrameTick());
    }
    // The general factory can create plugins and personal-data widgets; this
    // passive scene must never route such types to it, even with direct input.
    for (const auto* type : {"todo", "sticker", "login_box", "untrusted/plugin"}) {
      auto rejected = label; rejected.id="rejected"; rejected.type=type;
      layout.widgets.push_back(rejected);
      scene.sync(layout,"DP-1",true,renderer,800,600);
      TEST_CHECK(root.children().size()==2); retainedAuth();
      layout.widgets.pop_back();
    }
  }
  // Destroying the decoration manager detaches only its nodes.
  TEST_CHECK(root.children().size()==1);retainedAuth();
}

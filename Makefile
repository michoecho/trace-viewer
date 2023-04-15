CXXFLAGS = -O2 -g -I $(IMGUI_DIR)/include/imgui -I implot -std=c++20
LDLIBS = -lglfw -lGL -lm
all: main

imgui_impl%.o: $(IMGUI_DIR)/include/imgui/backends/imgui_impl%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ -c
imgu%.o: $(IMGUI_DIR)/include/imgui/imgu%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ -c
implo%.o: implot/implo%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ -o $@ -c

main: main.o imgui.o imgui_tables.o imgui_widgets.o imgui_impl_opengl3.o imgui_impl_glfw.o imgui_demo.o imgui_draw.o implot.o implot_items.o implot_demo.o
	$(CXX) $(LDLIBS) $(LDFLAGS) $^ -o $@

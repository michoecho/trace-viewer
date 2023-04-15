#include <implot.h>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_glfw.h>
#include <imgui/backends/imgui_impl_opengl3.h>
#include <algorithm>
#include <stdio.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <span>
#include <functional>
#include <chrono>
#include <thread>
#include <vector>
#include <cerrno>
#include <atomic>
#include <stdexcept>
#include <system_error>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <GLFW/glfw3.h> // Will drag system OpenGL headers

const double MULTIPLIER = 0.2941171840072451;

inline int64_t rdtsc() {
    uint64_t rax, rdx;
    asm volatile ( "rdtsc" : "=a" (rax), "=d" (rdx) );
    return (int64_t)(( rdx << 32 ) + rax);
}

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

struct entry {
    uint64_t event;
    uint64_t id;
    uint64_t arg;
    int64_t ts;

    uint64_t query() const {
        if (event == 0 || event == 1 || event == 0xa || event == 0xb) {
            return arg;
        } else {
            return id;
        }
    }
};
template <> struct fmt::formatter<entry> : formatter<string_view> {
    auto format(const entry& e, auto& ctx) const -> decltype(ctx.out()) {
        // ctx.out() is an output iterator to write to.
        return fmt::format_to(ctx.out(), "({:016x} {:016x} {:016x} {:016x})", e.event, e.id, e.arg, e.ts);
    }
};

int main(int argc, char** argv) {
    if (argc == 1) {
        throw std::runtime_error("USAGE: ./main FILE");
    }
    const char *memblock;
    int fd;
    struct stat sb;

    fd = open(argv[1], O_RDONLY);
    fstat(fd, &sb);
    size_t file_size = sb.st_size;
    memblock = (char*)mmap(nullptr, file_size, PROT_WRITE | PROT_READ, MAP_PRIVATE, fd, 0);
    if (memblock == MAP_FAILED) {
        throw std::system_error(errno, std::generic_category(), argv[1]);
    }
    size_t n_entries = file_size / sizeof(entry);
    auto span = std::span<const entry>(reinterpret_cast<const entry*>(memblock), n_entries);
    auto sorted = std::vector<entry>(span.begin(), span.end());
    std::ranges::sort(sorted, std::ranges::less(), [] (const auto &x) {return std::make_pair(x.query(), x.ts);});
#if 0
    for (const auto &x : sorted) {
        fmt::print("{:016x} {}\n", x.query(), x) ;
    }
#endif

    struct query {
        std::chrono::duration<double> latency;
        uint64_t id;
        std::chrono::duration<double> cputime;
        std::chrono::duration<double> iotime;
        std::chrono::duration<double> starvetime;
    };
    std::vector<query> queries;
    {
        size_t i = 0;
        while (i < sorted.size()) {
            while (sorted[i].event != 1 && i < sorted.size()) {
                ++i;
            }
            if (i == sorted.size()) {
                break;
            }
            auto current_query = sorted[i].query();
            auto start = sorted[i].ts;
            while (i + 1 < sorted.size() && sorted[i + 1].query() == current_query) {
                ++i;
            }
            auto end = sorted[i].ts;
            auto time = std::chrono::duration<double, std::nano>(double(end - start) * MULTIPLIER);
            queries.push_back(query{time, current_query});
            ++i;
        }
    }
    std::ranges::sort(queries, std::ranges::less(), [] (const auto &x) {return x.latency;});
#if 0
    for (const auto &x : queries) {
        fmt::print("{} {}\n", x.latency.count(), x.id) ;
    }
#endif

    {
        for (auto &x : queries) {
            auto id = x.id;
            auto sorted_range = std::ranges::equal_range(sorted, id, std::ranges::less(), [] (const auto& e) {return e.query();});
            //fmt::print("tsrange: {} {}\n", sorted_range.front().ts, sorted_range.back().ts);
            auto span_range = std::ranges::equal_range(span, 1, std::ranges::less(), [&sorted_range] (const auto& e) {return (e.ts >= sorted_range.front().ts) + (e.ts > sorted_range.back().ts);});

            uint64_t iostack = 0;
            bool cpu = true;
            uint64_t prev_ts = sorted_range.begin()->ts;
            uint64_t cputime = 0;
            uint64_t starvetime = 0;
            uint64_t iotime = 0;
            size_t i;
            //fmt::print("range: {} {}\n", span_range.begin() - span.begin(), span_range.end() - span.begin());
            for (i = span_range.begin() - span.begin(); i < size_t(span_range.end() - span.begin()); ++i) {
                //fmt::print("looping: {}\n", i);
                uint64_t dt = span[i].ts - prev_ts;
                if (iostack == 0 && !cpu) {
                    starvetime += dt;
                }
                if (cpu) {
                    cputime += dt;
                }
                if (iostack) {
                    iotime += dt;
                }
                if (span[i].query() == id) {
                    if (span[i].event != 0x5) {
                        cpu = true;
                    }
                    if (span[i].event == 0x4) {
                        iostack += 1;
                    } else if (span[i].event == 0x5) {
                        iostack -= 1;
                    }
                } else {
                    cpu = false;
                }
                prev_ts = span[i].ts;
            }
            auto conv = [] (uint64_t ticks) {
                return std::chrono::duration<double, std::nano>(ticks * MULTIPLIER);
            };
            x.iotime = conv(iotime);
            x.starvetime = conv(starvetime);
            x.cputime = conv(cputime);
            //fmt::print("cputime: {}", cputime);
        }
    }

    std::vector<double> xx;
    std::vector<double> yy;
    if (queries.size()) {
        for (int i = 0; i <= 1000; ++i) {
            double x = pow(100000.0, i/1000.0);
            size_t w = queries.size() - size_t(1.0 / x * queries.size());
            xx.push_back(x);
            yy.push_back(queries[std::clamp(w, size_t(0), queries.size() - 1)].latency.count());
        }
        for (size_t i = 0; i < xx.size(); ++i) {
            //fmt::print("{} {}\n", xx[i], yy[i]);
        }
    }

#if 0
    {
        double m_timerMul = 1.;

        std::atomic_signal_fence( std::memory_order_acq_rel );
        const auto t0 = std::chrono::high_resolution_clock::now();
        const auto r0 = rdtsc();
        std::atomic_signal_fence( std::memory_order_acq_rel );
        std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
        std::atomic_signal_fence( std::memory_order_acq_rel );
        const auto t1 = std::chrono::high_resolution_clock::now();
        const auto r1 = rdtsc();
        std::atomic_signal_fence( std::memory_order_acq_rel );

        const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>( t1 - t0 ).count();
        const auto dr = r1 - r0;

        m_timerMul = double( dt ) / double( dr );
        fmt::print("dt: {}, dr: {}, MULT: {}\n", dt, dr, m_timerMul);
    }
#endif

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Latency analyzer", nullptr, nullptr);
    if (window == nullptr)
        return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    // - Our Emscripten build process allows embedding fonts to be accessible at runtime from the "fonts/" folder. See Makefile.emscripten for details.
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != nullptr);

    // Our state
    bool show_demo_window = true;
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        // Poll and handle events (inputs, window resize, etc.)
        // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
        // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
        // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
        // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
        if (show_demo_window) {
            // ImGui::ShowDemoWindow(&show_demo_window);
        }

#if 0
        // 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
        {
            static float f = 0.0f;
            static int counter = 0;

            ImGui::Begin("Hello, world!");                          // Create a window called "Hello, world!" and append into it.

            ImGui::Text("This is some useful text.");               // Display some text (you can use a format strings too)
            ImGui::Checkbox("Demo Window", &show_demo_window);      // Edit bools storing our window open/close state
            ImGui::Checkbox("Another Window", &show_another_window);

            ImGui::SliderFloat("float", &f, 0.0f, 1.0f);            // Edit 1 float using a slider from 0.0f to 1.0f
            ImGui::ColorEdit3("clear color", (float*)&clear_color); // Edit 3 floats representing a color

            if (ImGui::Button("Button"))                            // Buttons return true when clicked (most widgets return true when edited/activated)
                counter++;
            ImGui::SameLine();
            ImGui::Text("counter = %d", counter);

            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);

            {
                static bool animate = true;
                ImGui::Checkbox("Animate", &animate);

                // Plot as lines and plot as histogram
                //IMGUI_DEMO_MARKER("Widgets/Plotting/PlotLines, PlotHistogram");
                static float arr[] = { 0.6f, 0.1f, 1.0f, 0.5f, 0.92f, 0.1f, 0.2f };
                ImGui::PlotLines("Frame Times", arr, IM_ARRAYSIZE(arr));
                ImGui::PlotHistogram("Histogram", arr, IM_ARRAYSIZE(arr), 0, NULL, 0.0f, 1.0f, ImVec2(0, 80.0f));

                // Fill an array of contiguous float values to plot
                // Tip: If your float aren't contiguous but part of a structure, you can pass a pointer to your first float
                // and the sizeof() of your structure in the "stride" parameter.
                static float values[90] = {};
                static int values_offset = 0;
                static double refresh_time = 0.0;
                if (!animate || refresh_time == 0.0)
                    refresh_time = ImGui::GetTime();
                while (refresh_time < ImGui::GetTime()) // Create data at fixed 60 Hz rate for the demo
                {
                    static float phase = 0.0f;
                    values[values_offset] = cosf(phase);
                    values_offset = (values_offset + 1) % IM_ARRAYSIZE(values);
                    phase += 0.10f * values_offset;
                    refresh_time += 1.0f / 60.0f;
                }

                // Plots can display overlay texts
                // (in this example, we will display an average value)
                {
                    float average = 0.0f;
                    for (int n = 0; n < IM_ARRAYSIZE(values); n++)
                        average += values[n];
                    average /= (float)IM_ARRAYSIZE(values);
                    char overlay[32];
                    sprintf(overlay, "avg %f", average);
                    ImGui::PlotLines("Lines", values, IM_ARRAYSIZE(values), values_offset, overlay, -1.0f, 1.0f, ImVec2(0, 80.0f));
                }

                // Use functions to generate output
                // FIXME: This is rather awkward because current plot API only pass in indices.
                // We probably want an API passing floats and user provide sample rate/count.
                struct Funcs
                {
                    static float Sin(void*, int i) { return sinf(i * 0.1f); }
                    static float Saw(void*, int i) { return (i & 1) ? 1.0f : -1.0f; }
                };
                static int func_type = 0, display_count = 70;
                ImGui::Separator();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
                ImGui::Combo("func", &func_type, "Sin\0Saw\0");
                ImGui::SameLine();
                ImGui::SliderInt("Sample count", &display_count, 1, 400);
                float (*func)(void*, int) = (func_type == 0) ? Funcs::Sin : Funcs::Saw;
                ImGui::PlotLines("Lines", func, NULL, display_count, 0, NULL, -1.0f, 1.0f, ImVec2(0, 80));
                ImGui::PlotHistogram("Histogram", func, NULL, display_count, 0, NULL, -1.0f, 1.0f, ImVec2(0, 80));
                ImGui::Separator();

                // Animate a simple progress bar
                //IMGUI_DEMO_MARKER("Widgets/Plotting/ProgressBar");
                static float progress = 0.0f, progress_dir = 1.0f;
                if (animate)
                {
                    progress += progress_dir * 0.4f * ImGui::GetIO().DeltaTime;
                    if (progress >= +1.1f) { progress = +1.1f; progress_dir *= -1.0f; }
                    if (progress <= -0.1f) { progress = -0.1f; progress_dir *= -1.0f; }
                }

                // Typically we would use ImVec2(-1.0f,0.0f) or ImVec2(-FLT_MIN,0.0f) to use all available width,
                // or ImVec2(width,0.0f) for a specified width. ImVec2(0.0f,0.0f) uses ItemWidth.
                ImGui::ProgressBar(progress, ImVec2(0.0f, 0.0f));
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::Text("Progress Bar");

                float progress_saturated = std::clamp(progress, 0.0f, 1.0f);
                char buf[32];
                sprintf(buf, "%d/%d", (int)(progress_saturated * 1753), 1753);
                ImGui::ProgressBar(progress, ImVec2(0.f, 0.f), buf);
            }
            ImGui::End();
        }
#endif

        //ImPlot::ShowDemoWindow();

#if 0
        // 3. Show another simple window.
        if (show_another_window)
        {
            ImGui::Begin("Another Window", &show_another_window);   // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
            ImGui::Text("Hello from another window!");
            if (ImGui::Button("Close Me"))
                show_another_window = false;
            ImGui::End();
        }
#endif

        static size_t chosen_one = -1;
        static bool just_chosen = true;
        static size_t chosen_unfull = -1;
        static bool just_chosen_unfull = true;
        static uint64_t id_log = queries[0].id;
        {
            ImGui::Begin("Graph");
            ImPlot::BeginPlot("HdrHistogram", ImVec2(-1,0));
            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_Lock, ImPlotAxisFlags_Lock);
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            ImPlot::SetupAxesLimits(1, 100000, 0.0001, queries.back().latency.count());
            if (ImPlot::IsPlotSelected()) {
                static ImPlotRect limits, select;
                select = ImPlot::GetPlotSelection();
            }
            ImPlot::PlotLine("Latency", xx.data(), yy.data(), 1001);
            static double line_x;
            static size_t w = 0;
            static uint64_t id_full_log = id_log;

            if (ImPlot::IsPlotHovered() && ImGui::IsMouseDown(0)) {
                ImPlotPoint pt = ImPlot::GetPlotMousePos();
                line_x = std::clamp(pt.x, 1.0, 100000.0);
                w = std::clamp(queries.size() - size_t(1.0 / line_x * queries.size()), size_t(0), size_t(queries.size() - 1));
                id_log = queries[w].id;
                id_full_log = id_log;
            }
            ImPlotDragToolFlags flags = ImPlotDragToolFlags_NoCursors | ImPlotDragToolFlags_NoFit | ImPlotDragToolFlags_NoInputs;
            ImPlot::DragLineX(0, &line_x, ImVec4(1,1,1,1), 1, flags);

            static double rect[] = {100.0, 0.001, 141.2, 0.003};
            rect[1] = 0.0001;
            rect[3] = 0.001;
            ImPlot::DragRect(0,&rect[0],&rect[1],&rect[2],&rect[3],ImVec4(1,0,1,1), ImPlotDragToolFlags_Delayed);

            ImPlot::EndPlot();

            ImGui::End();

            ImGui::Begin("TimeDist");
            {
                static size_t w1g = -1;
                static size_t w2g = -1;
                size_t w1 = std::clamp(queries.size() - size_t(1.0 / rect[0] * queries.size()), size_t(0), size_t(queries.size() - 1));
                size_t w2 = std::clamp(queries.size() - size_t(1.0 / rect[2] * queries.size()), size_t(0), size_t(queries.size() - 1));
                using t = std::chrono::duration<double>;
                static std::vector<double> plot_x = std::invoke([&] {
                    std::vector<double> v;
                    for (int i = 0; i < 1024; ++i) {
                        v.push_back(i * (1.0/1024));
                    }
                    return v;
                });
                static std::vector<double> iotimes_y, cputimes_y, latencies_y, starvetimes_y;
                static t avgiotime, avgcputime, avgstarvetime, avglatency;

                if (w1 != w1g || w2 != w2g) {
                    w1g = w1;
                    w2g = w2;
                    avgiotime = avgcputime = avglatency = avgstarvetime = t::zero();
                    std::vector<t> iotimes, cputimes, latencies, starvetimes;
                    for (size_t i = w1; i <= w2; ++i) {
                        avgiotime += queries[i].iotime / (w2 - w1 + 1);
                        avgcputime += queries[i].cputime / (w2 - w1 + 1);
                        avgstarvetime += queries[i].starvetime / (w2 - w1 + 1);
                        avglatency += queries[i].latency / (w2 - w1 + 1);

                        iotimes.push_back(queries[i].iotime);
                        cputimes.push_back(queries[i].cputime);
                        starvetimes.push_back(queries[i].starvetime);
                        latencies.push_back(queries[i].latency);
                    }
                    std::ranges::sort(iotimes);
                    std::ranges::sort(cputimes);
                    std::ranges::sort(starvetimes);
                    std::ranges::sort(latencies);

                    auto sample = [&] (std::vector<t>& vec) {
                        auto res = std::vector<double>();
                        if (vec.empty()) {
                            return res;
                        }
                        for (const auto& p : plot_x) {
                            size_t ww = (vec.size() - 1) * p;
                            res.push_back(std::chrono::duration<double, std::milli>(vec[ww]).count());
                        }
                        return res;
                    };
                    iotimes_y = sample(iotimes);
                    starvetimes_y = sample(starvetimes);
                    cputimes_y = sample(cputimes);
                    latencies_y = sample(latencies);
                }

                ImGui::Text("%s", fmt::format("{:10s} {:12.9f}", "CPU", std::chrono::duration<double, std::milli>(avgcputime).count()).c_str());
                ImGui::Text("%s", fmt::format("{:10s} {:12.9f}", "STARVE", std::chrono::duration<double, std::milli>(avgstarvetime).count()).c_str());
                ImGui::Text("%s", fmt::format("{:10s} {:12.9f}", "IO", std::chrono::duration<double, std::milli>(avgiotime).count()).c_str());
                ImGui::Text("%s", fmt::format("{:10s} {:12.9f}", "TOTAL", std::chrono::duration<double, std::milli>(avglatency).count()).c_str());

                if (ImPlot::BeginSubplots("My Subplot",2,2,ImVec2(-1, -1))) {
                    if (ImPlot::BeginPlot("iotime cdf", ImVec2(-1,0))) {
                        ImPlot::SetupAxes(NULL,NULL,0,ImPlotAxisFlags_AutoFit|ImPlotAxisFlags_RangeFit);
                        ImPlot::PlotLine("iotime cdf", plot_x.data(), iotimes_y.data(), iotimes_y.size());
                        ImPlot::EndPlot();
                    }
                    if (ImPlot::BeginPlot("starvetime cdf", ImVec2(-1,0))) {
                        ImPlot::SetupAxes(NULL,NULL,0,ImPlotAxisFlags_AutoFit|ImPlotAxisFlags_RangeFit);
                        ImPlot::PlotLine("starvetime cdf", plot_x.data(), starvetimes_y.data(), starvetimes_y.size());
                        ImPlot::EndPlot();
                    }
                    if (ImPlot::BeginPlot("cputime cdf", ImVec2(-1,0))) {
                        ImPlot::SetupAxes(NULL,NULL,0,ImPlotAxisFlags_AutoFit|ImPlotAxisFlags_RangeFit);
                        ImPlot::PlotLine("cputime cdf", plot_x.data(), cputimes_y.data(), cputimes_y.size());
                        ImPlot::EndPlot();
                    }
                    if (ImPlot::BeginPlot("latency cdf", ImVec2(-1,0))) {
                        ImPlot::SetupAxes(NULL,NULL,0,ImPlotAxisFlags_AutoFit|ImPlotAxisFlags_RangeFit);
                        ImPlot::PlotLine("latency cdf", plot_x.data(), latencies_y.data(), latencies_y.size());
                        ImPlot::EndPlot();
                    }
                    ImPlot::EndSubplots();
                }
            }
            ImGui::End();

            {
                ImGui::Begin("Log");
                ImGui::Text("%s", fmt::format("{:10s} {:12.9f}", "CPU", std::chrono::duration<double, std::milli>(queries[w].cputime).count()).c_str());
                ImGui::Text("%s", fmt::format("{:10s} {:12.9f}", "STARVE", std::chrono::duration<double, std::milli>(queries[w].starvetime).count()).c_str());
                ImGui::Text("%s", fmt::format("{:10s} {:12.9f}", "IO", std::chrono::duration<double, std::milli>(queries[w].iotime).count()).c_str());
                ImGui::Text("%s", fmt::format("{:10s} {:12.9f}", "TOTAL", std::chrono::duration<double, std::milli>(queries[w].latency).count()).c_str());
                uint64_t start = std::ranges::lower_bound(sorted, id_log, std::ranges::less(), [] (const auto& e) {return e.query();}) - sorted.begin();
                uint64_t end = std::ranges::upper_bound(sorted, id_log, std::ranges::less(), [] (const auto& e) {return e.query();}) - sorted.begin() - 1;
                uint64_t start_ts = sorted[start].ts;
                //uint64_t end_ts = sorted[end].ts;
                static size_t selected = 0;
                for (size_t i = start; i <= end; ++i) {
                    auto dt_nano = std::chrono::duration<double, std::nano>(double(sorted[i].ts - start_ts) * MULTIPLIER);
                    auto dt = std::chrono::duration<double, std::milli>(dt_nano);
                    auto message = std::invoke([&] () -> std::string {
                        const auto e = sorted[i];
                        switch (e.event) {
                        case 0: return fmt::format("{:10s}", "SWITCH");
                        case 1: return "START";
                        case 0xa: return "PERMIT";
                        case 0xb: return "ES";
                        case 0x3: {
                        const char* rcs_status[] = {
                        "admitted immediately",
                        "queued because of non-empty ready",
                        "queued because of used permits",
                        "queued because of memory resources",
                        "queued because of count resources",
                        };
                        return fmt::format("{:10s} {}", "RCS", rcs_status[e.arg]);
                        }
                        case 0x4: return fmt::format("{:10s} {:16x}", "IO_BEGIN", e.arg);
                        case 0x5: return fmt::format("{:10s} {:16x}", "IO_END", e.arg);
                        default: return fmt::format("UNKNOWN ({})", e.event);
                        }
                    });
                    bool highlighted = (selected >= start && selected <= end) && (sorted[i].event == 0x4 || sorted[i].event == 0x5) && (sorted[i].arg == sorted[selected].arg);
                    auto s = fmt::format("{:12.9f}: {}", dt.count(), message);
                    if (i == chosen_unfull) {
                        if (just_chosen_unfull) {
                            just_chosen_unfull = false;
                            ImGui::SetScrollHereY();
                        }
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.f, 0.0f, 1.f));
                    }
                    if (ImGui::Selectable(s.c_str(), highlighted)) {
                        if (highlighted) {
                            selected = -1;
                        } else {
                            selected = i;
                        }
                    }
                    if (i == chosen_unfull) {
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::End();
            }
#if 1
            {
                ImGui::Begin("Full log");
                uint64_t start = std::ranges::lower_bound(sorted, id_full_log, std::ranges::less(), [] (const auto& e) {return e.query();}) - sorted.begin();
                uint64_t end = std::ranges::upper_bound(sorted, id_full_log, std::ranges::less(), [] (const auto& e) {return e.query();}) - sorted.begin() - 1;
                uint64_t start_ts = sorted[start].ts;
                uint64_t end_ts = sorted[end].ts;
                start = std::ranges::lower_bound(span, start_ts, std::ranges::less(), [] (const auto& e) {return e.ts;}) - span.begin();
                end = std::ranges::lower_bound(span, end_ts, std::ranges::less(), [] (const auto& e) {return e.ts;}) - span.begin() - 1;
                static size_t selected = 0;
                for (size_t i = start; i <= end; ++i) {
                    auto dt_nano = std::chrono::duration<double, std::nano>(double(span[i].ts - start_ts) * MULTIPLIER);
                    auto dt = std::chrono::duration<double, std::milli>(dt_nano);
                    auto message = std::invoke([&] () -> std::string {
                        const auto e = span[i];
                        switch (e.event) {
                        case 0: return fmt::format("{:10s}", "SWITCH");
                        case 1: return "START";
                        case 0xa: return "PERMIT";
                        case 0xb: return "ES";
                        case 0x3: {
                        const char* rcs_status[] = {
                        "admitted immediately",
                        "queued because of non-empty ready",
                        "queued because of used permits",
                        "queued because of memory resources",
                        "queued because of count resources",
                        };
                        return fmt::format("{:10s} {}", "RCS", rcs_status[e.arg]);
                        }
                        case 0x4: return fmt::format("{:10s} {:16x}", "IO_BEGIN", e.arg);
                        case 0x5: return fmt::format("{:10s} {:16x}", "IO_END", e.arg);
                        default: return fmt::format("UNKNOWN ({})", e.event);
                        }
                    });
                    //bool highlighted = (selected >= start && selected <= end) && (span[i].event == 0x4 || span[i].event == 0x5) && (span[i].arg == span[selected].arg);
                    bool highlighted = (selected >= start && selected <= end) && (span[i].query() == id_log);
                    auto s = fmt::format("{:12.9f}: {:16x}: {}", dt.count(), span[i].query(), message);
                    bool is_active = span[i].query() == id_full_log;
                    if (is_active) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.f, 1.f, 0.24f, 1.f));
                    }
                    if (i == chosen_one) {
                        if (just_chosen) {
                            just_chosen = false;
                            ImGui::SetScrollHereY();
                        }
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.f, 0.0f, 1.f));
                    }
                    if (ImGui::Selectable(s.c_str(), highlighted)) {
                        auto x = span[i].query();
                        if (x) {
                            id_log = x;
                        }
                        if (highlighted) {
                            selected = -1;
                        } else {
                            selected = i;
                        }
                    }
                    if (i == chosen_one) {
                        ImGui::PopStyleColor();
                    }
                    if (is_active) {
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::End();
            }
#endif
            {
                ImGui::Begin("Full log plot");
                if (ImPlot::BeginPlot("Full log plot", ImVec2(-1, 100), ImPlotFlags_NoTitle)) {
                    static uint64_t prev_id;
                    auto flag = prev_id == id_full_log ? ImPlotCond_Once : ImPlotCond_Always;
                    prev_id = id_full_log;

                    auto sorted_range = std::ranges::equal_range(sorted, id_full_log, std::ranges::less(), [] (const auto& e) {return e.query();});
                    auto span_range = std::ranges::equal_range(span, 1, std::ranges::less(), [&sorted_range] (const auto& e) {return (e.ts >= sorted_range.front().ts) + (e.ts > sorted_range.back().ts);});

                    uint64_t start_ts = sorted_range.front().ts;
                    uint64_t end_ts = sorted_range.back().ts;
                    ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_NoGridLines, ImPlotAxisFlags_Lock | ImPlotAxisFlags_NoDecorations);
                    ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, 0, double(end_ts - start_ts)*MULTIPLIER/1e6);
                    ImPlot::SetupAxesLimits(0, double(end_ts - start_ts)*MULTIPLIER/1e6, 0, 1, flag);
                    ImPlot::PushPlotClipRect();

                    uint64_t prev_ts = start_ts;
                    bool cpu = true;
                    uint64_t iostack = 0;
                    uint64_t iostart = 0;
                    for (const auto& x : span_range) {
                            ImVec2 rmin = ImPlot::PlotToPixels(ImPlotPoint(double(prev_ts - start_ts)*MULTIPLIER/1e6, 1.f));
                            ImVec2 rmax = ImPlot::PlotToPixels(ImPlotPoint(double(x.ts - start_ts)*MULTIPLIER/1e6, 0.f));
                            ImVec2 rmin_low = ImPlot::PlotToPixels(ImPlotPoint(double(prev_ts - start_ts)*MULTIPLIER/1e6, 0.f));
                        if (cpu) {
                            ImPlot::GetPlotDrawList()->AddLine(rmin, rmin_low, IM_COL32(0,128,0,255));
                            ImPlot::GetPlotDrawList()->AddRectFilled(rmin, rmax, IM_COL32(0,128,0,255));
                        } else {
                            ImPlot::GetPlotDrawList()->AddRectFilled(rmin, rmax, IM_COL32(0,0,128,32));
                        }
                        if (x.query() == id_full_log) {
                            if (x.event != 0x5) {
                                cpu = true;
                            }
                            if (x.event == 0x4) {
                                if (iostack == 0) {
                                    iostart = x.ts;
                                }
                                iostack += 1;
                            } else if (x.event == 0x5) {
                                iostack -= 1;
                                if (iostack == 0) {
                                    ImVec2 rmin = ImPlot::PlotToPixels(ImPlotPoint(double(iostart - start_ts)*MULTIPLIER/1e6, 1.f));
                                    ImVec2 rmax = ImPlot::PlotToPixels(ImPlotPoint(double(x.ts - start_ts)*MULTIPLIER/1e6, 0.f));
                                    ImPlot::GetPlotDrawList()->AddRectFilled(rmin, rmax, IM_COL32(255,255,255,32));
                                }
                            }
                        } else {
                            cpu = false;
                        }
                        prev_ts = x.ts;
                    }
                    ImPlot::PopPlotClipRect();

                    if (ImPlot::IsPlotHovered() && ImGui::IsMouseDown(0)) {
                        ImPlotPoint pt = ImPlot::GetPlotMousePos();
                        uint64_t ts = start_ts + pt.x * 1e6 / MULTIPLIER;
                        chosen_one = std::ranges::lower_bound(span, ts, std::ranges::less(), [] (const auto& e) {return e.ts;}) - span.begin() - 1;
                        chosen_one = std::clamp(chosen_one, size_t(0), span.size() - 1);
                        just_chosen = true;
                        chosen_unfull = std::ranges::lower_bound(sorted, std::make_pair<uint64_t, uint64_t>(uint64_t(id_log), uint64_t(ts)), std::ranges::less(), [] (const auto& e) {return std::make_pair<uint64_t, uint64_t>(e.query(), e.ts);}) - sorted.begin() - 1;
                        chosen_unfull = std::clamp(chosen_unfull, size_t(0), sorted.size() - 1);
                        just_chosen_unfull = true;
                    }
                    ImPlot::EndPlot();
                }
                ImGui::End();
            }

        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

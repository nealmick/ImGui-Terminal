#define GL_SILENCE_DEPRECATION
#define GLEW_NO_GLU
#include <GL/glew.h>
#if defined(__APPLE__)
#define GL_GLEXT_PROTOTYPES 1
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <string>
#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <unistd.h>
#include <chrono>
#include "terminal.h"
#include "shaders/shader.h"


float Clamp(float value, float min, float max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

ImFont* LoadFont(const std::string& fontName, float fontSize) {
    ImGuiIO& io = ImGui::GetIO();
    std::string fontPath = "fonts/" + fontName + ".ttf";
    
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        std::cout << "\033[32mMain:\033[0m opening working directory : " << cwd << std::endl;
    } else {
        std::cerr << "getcwd() error" << std::endl;
    }
    
    std::cout << "\033[32mMain:\033[0m Attempting to load font from: " << fontPath << std::endl;
    if (!std::filesystem::exists(fontPath)) {
        std::cerr << "\033[32mMain:\033[0m Font file does not exist: " << fontPath << std::endl;
        return io.Fonts->AddFontDefault();
    }

    static const ImWchar ranges[] = {
        0x0020, 0x00FF,  // Basic Latin + Latin Supplement
        0x2500, 0x257F,  // Box Drawing Characters
        0x2580, 0x259F,  // Block Elements
        0x25A0, 0x25FF,  // Geometric Shapes
        0x2600, 0x26FF,  // Miscellaneous Symbols
        0x2700, 0x27BF,  // Dingbats
        0x2900, 0x297F,  // Supplemental Arrows-B
        0x2B00, 0x2BFF,  // Miscellaneous Symbols and Arrows
        0x3000, 0x303F,  // CJK Symbols and Punctuation
        0xE000, 0xE0FF,  // Private Use Area
        0,
    };

    // Create config for the main font (from settings)
    ImFontConfig config_main;
    config_main.MergeMode = false;
    config_main.GlyphRanges = ranges;

    // Load the main font first (from settings)
    ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSize, &config_main, ranges);

    // Then merge DejaVu Sans just for the Braille range
    ImFontConfig config_braille;
    config_braille.MergeMode = true;  // Important! This will merge with previous font
    static const ImWchar braille_ranges[] = { 0x2800, 0x28FF, 0 };
    io.Fonts->AddFontFromFileTTF("fonts/DejaVuSans.ttf", fontSize, &config_braille, braille_ranges);
    
    if (font == nullptr) {
        std::cerr << "\033[32mMain:\033[0m Failed to load font: " << fontName << std::endl;
        return io.Fonts->AddFontDefault();
    }
    std::cout << "\033[32mMain:\033[0m Successfully loaded font: " << fontName << std::endl;
    return font;
}

void InitializeGLFW() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        exit(1);
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    #ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    #endif
}

GLFWwindow* CreateWindow() {
    GLFWwindow* window = glfwCreateWindow(1200, 750, "TERD", NULL, NULL);
    if (window == NULL) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        exit(1);
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    return window;
}

void InitializeImGui(GLFWwindow* window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::cout << "Initializing terminal..." << std::endl;
    gTerminal.toggleVisibility(); // Make sure terminal is visible
}





int main() {
    // Initialize GLFW
    InitializeGLFW();
    
    // Create window
   GLFWwindow* window = CreateWindow();
    glfwSetWindowRefreshCallback(window, [](GLFWwindow* window) {
        glfwPostEmptyEvent();
    });
    
    // Initialize GLEW
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (GLEW_OK != err) {
        std::cerr << "GLEW initialization failed: " << glewGetErrorString(err) << std::endl;
        glfwTerminate();
        return -1;
    }
    glGetError(); // Clear any error that GLEW init might have caused

    // Initialize ImGui
    InitializeImGui(window);
    

	ImFont* currentFont = LoadFont("SourceCodePro-Regular", 18.0f);
    if (currentFont == nullptr) {
        std::cerr << "Failed to load font, using default font" << std::endl;
        currentFont = ImGui::GetIO().Fonts->AddFontDefault();
    }

    // Shader and Framebuffer setup
    Shader crtShader;
    if (!crtShader.loadShader("shaders/vertex.glsl", "shaders/fragment.glsl")) {
        std::cerr << "Shader load failed" << std::endl;
        glfwTerminate();
        return -1;
    }

    // Persistent framebuffer objects
    GLuint fullFramebuffer = 0, fullRenderTexture = 0, fullRbo = 0;
    int last_display_w = 0, last_display_h = 0;
    bool frameBufferInitialized = false;

    // Frame timing and performance tracking
    int frameCount = 0;
    double lastFPSTime = glfwGetTime();

    // Performance constants
    const double TARGET_FPS = 60.0;
    const std::chrono::duration<double> TARGET_FRAME_DURATION(1.0 / TARGET_FPS);

    // Render state tracking
    bool is_window_moving = false;
    bool needFontReload = false;
    bool windowFocused = true;

    // Quad vertices for shader rendering
    float quadVertices[] = {
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 1.0f
    };

    // Setup quad VAO and VBO
    GLuint quadVAO, quadVBO;
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &quadVBO);
    glBindVertexArray(quadVAO);
    glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);

    // Position attribute
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    // Texture coordinate attribute
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);


    while (!glfwWindowShouldClose(window)) {
        auto frame_start = std::chrono::high_resolution_clock::now();

        // Always poll events
        if (glfwGetWindowAttrib(window, GLFW_FOCUSED)) {
            // Use polling when focused for responsive input
            glfwPollEvents();
        } else {
            // Use event waiting with timeout when not focused
            glfwWaitEventsTimeout(0.016);  // 16ms ~60Hz timeout
        }

        // Performance tracking
        double currentTime = glfwGetTime();
        frameCount++;
        
        // FPS reporting
        /*
        if (currentTime - lastFPSTime >= 1.0) {
            // Color codes
            const char* color = "\033[31m";  // Default to red
            
            if (frameCount >= 50) {
                color = "\033[32m";  // Green for 50+ FPS
            } else if (frameCount >= 30) {
                color = "\033[33m";  // Orange (yellow) for 30-49 FPS
            }
            
            std::cout << color << "FPS: " << frameCount << "\033[0m" << std::endl;
            frameCount = 0;
            lastFPSTime += 1.0;
        }
        */
        // Get framebuffer size
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);

        // Reinitialize framebuffer if window size changes
        if (display_w != last_display_w || display_h != last_display_h || !frameBufferInitialized) {
            if (frameBufferInitialized) {
                glDeleteFramebuffers(1, &fullFramebuffer);
                glDeleteTextures(1, &fullRenderTexture);
                glDeleteRenderbuffers(1, &fullRbo);
            }

            glGenFramebuffers(1, &fullFramebuffer);
            glGenTextures(1, &fullRenderTexture);
            glGenRenderbuffers(1, &fullRbo);

            glBindFramebuffer(GL_FRAMEBUFFER, fullFramebuffer);
            glBindTexture(GL_TEXTURE_2D, fullRenderTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, display_w, display_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fullRenderTexture, 0);

            glBindRenderbuffer(GL_RENDERBUFFER, fullRbo);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, display_w, display_h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fullRbo);

            last_display_w = display_w;
            last_display_h = display_h;
            frameBufferInitialized = true;
        }

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();


        // Render to default framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // Set window background color
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.15f, 0.2f, 1.0f)); // Dark bluish-grey
        // Render ImGui terminal
        gTerminal.render();
        ImGui::PopStyleColor();

        
        // Complete ImGui frame
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Rest of the shader rendering remains the same as in previous optimized version
        // (Copy framebuffer, apply shader effect)
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fullFramebuffer);
        glBlitFramebuffer(0, 0, display_w, display_h, 
                         0, 0, display_w, display_h,
                         GL_COLOR_BUFFER_BIT, GL_NEAREST);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClear(GL_COLOR_BUFFER_BIT);

        glUseProgram(crtShader.shaderProgram);
        
        GLint timeLocation = glGetUniformLocation(crtShader.shaderProgram, "time");
        GLint screenTextureLocation = glGetUniformLocation(crtShader.shaderProgram, "screenTexture");
        GLint resolutionLocation = glGetUniformLocation(crtShader.shaderProgram, "resolution");

        if (timeLocation != -1) glUniform1f(timeLocation, currentTime);
        if (screenTextureLocation != -1) glUniform1i(screenTextureLocation, 0);
        if (resolutionLocation != -1) {
            glUniform2f(resolutionLocation, static_cast<float>(display_w), static_cast<float>(display_h));
        }

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fullRenderTexture);
        glBindVertexArray(quadVAO);
        glDrawArrays(GL_TRIANGLES, 0, 6);


        // Swap buffers
        glfwSwapBuffers(window);

        // Frame timing
        auto frame_end = std::chrono::high_resolution_clock::now();
        auto frame_duration = frame_end - frame_start;
        
        // Optional: Add sleep to maintain target FPS
        std::this_thread::sleep_for(TARGET_FRAME_DURATION - frame_duration);
    }

    // Cleanup
    glDeleteVertexArrays(1, &quadVAO);
    glDeleteBuffers(1, &quadVBO);
    if (frameBufferInitialized) {
        glDeleteFramebuffers(1, &fullFramebuffer);
        glDeleteTextures(1, &fullRenderTexture);
        glDeleteRenderbuffers(1, &fullRbo);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

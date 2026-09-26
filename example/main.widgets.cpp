#include <hrl/hrl.h>

#include <GLFW/glfw3.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <cstdio>

static void FramebufferResize(GLFWwindow*, int width, int height)
{
    // HRL uses framebuffer/drawable pixels internally.
    // This keeps all viewports and widgets responsive to window resizing.
    HRL_WindowResizeCallback(width, height);
}

static void MouseMove(GLFWwindow* window, double x, double y)
{
    // HRL_MouseMovedCallback expects framebuffer coordinates. GLFW cursor
    // coordinates are window/client coordinates, so convert them for HiDPI.
    int windowWidth = 0, windowHeight = 0;
    int framebufferWidth = 0, framebufferHeight = 0;
    glfwGetWindowSize(window, &windowWidth, &windowHeight);
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    const float sx = (windowWidth > 0)
        ? static_cast<float>(framebufferWidth) / static_cast<float>(windowWidth)
        : 1.0f;
    const float sy = (windowHeight > 0)
        ? static_cast<float>(framebufferHeight) / static_cast<float>(windowHeight)
        : 1.0f;

    HRL_MouseMovedCallback(
        static_cast<float>(x) * sx,
        static_cast<float>(y) * sy
    );
}

static void MouseButton(GLFWwindow*, int button, int action, int)
{
    if (button != GLFW_MOUSE_BUTTON_LEFT)
        return;

    HRL_MouseButtonCallback(
        HRL_MOUSE_BUTTON_LEFT,
        action == GLFW_PRESS ? HRL_MOUSE_PRESS : HRL_MOUSE_RELEASE
    );
}

static void OnSlider(HRL_id, float value, void* userData)
{
    const HRL_id progress = *static_cast<HRL_id*>(userData);
    HRL_SetProgressBarValue(progress, value);
}

static void OnButton(HRL_id, int clicked, int released, void* userData)
{
    if (!released || !clicked)
        return;

    const HRL_id progress = *static_cast<HRL_id*>(userData);
    float value = HRL_GetProgressBarValue(progress) + 0.1f;
    if (value > 1.0f)
        value = 0.0f;
    HRL_SetProgressBarValue(progress, value);
}

static void OnCheckbox(HRL_id, int checked, void*)
{
    std::printf("Checkbox: %s\n", checked ? "checked" : "unchecked");
}

int main()
{
    if (!glfwInit())
        return 1;

    GLFWwindow* window = glfwCreateWindow(
        1280, 720, "HRL Responsive Widgets", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);

    HRL_Init(HRL_OPENGL_33);
    HRL_InitContext(
        static_cast<HRL_uint>(framebufferWidth),
        static_cast<HRL_uint>(framebufferHeight),
        reinterpret_cast<void*>(glfwGetProcAddress)
    );

    glfwSetFramebufferSizeCallback(window, FramebufferResize);
    glfwSetCursorPosCallback(window, MouseMove);
    glfwSetMouseButtonCallback(window, MouseButton);

    HRL_id scene = HRL_CreateScene(HRL_TRUE);
    HRL_id camera = HRL_CreateCamera(scene, HRL_PERSPECTIVE);
    HRL_id viewport = HRL_CreateViewport(scene, camera, 0.0f, 0.0f, 1.0f, 1.0f);

    std::ifstream fontFile("FiraCode-Bold.ttf", std::ios::binary);
    std::vector<char> fontData(
        (std::istreambuf_iterator<char>(fontFile)),
        std::istreambuf_iterator<char>()
    );

    HRL_id font = HRL_INVALID_ID;
    if (!fontData.empty())
    {
        font = HRL_CreateFont(fontData.data(), fontData.size());
    }

    // --------------------------------------------------------
    // Centered title
    // anchor = the point INSIDE the widget used by position.
    // --------------------------------------------------------
    HRL_id title = HRL_CreateWidget(viewport, HRL_WIDGET_LABEL);
    HRL_SetWidgetPosition(title, 0.5f, 0.07f);
    HRL_SetWidgetSize(title, 0.45f, 0.07f);
    HRL_SetWidgetAnchor(title, 0.5f, 0.5f);
    HRL_SetLabelText(title, "HRL Responsive Widgets");
    HRL_SetLabelTextSize(title, 28.0f);
    if (font != HRL_INVALID_ID)
        HRL_SetLabelFont(title, font);

    // --------------------------------------------------------
    // Slider + progress bar
    // --------------------------------------------------------
    HRL_id progress = HRL_CreateWidget(viewport, HRL_WIDGET_PROGRESSBAR);
    HRL_SetWidgetPosition(progress, 0.5f, 0.28f);
    HRL_SetWidgetSize(progress, 0.50f, 0.045f);
    HRL_SetWidgetAnchor(progress, 0.5f, 0.5f);
    HRL_SetProgressBarValue(progress, 0.25f);
    HRL_SetProgressBarBackgroundColor(progress, 0.12f, 0.12f, 0.12f, 1.0f);
    HRL_SetProgressBarFillColor(progress, 0.20f, 0.70f, 1.0f, 1.0f);

    HRL_id slider = HRL_CreateWidget(viewport, HRL_WIDGET_SLIDER);
    HRL_SetWidgetPosition(slider, 0.5f, 0.20f);
    HRL_SetWidgetSize(slider, 0.50f, 0.045f);
    HRL_SetWidgetAnchor(slider, 0.5f, 0.5f);
    HRL_SetSliderRange(slider, 0.0f, 1.0f);
    HRL_SetSliderValue(slider, 0.25f);
    HRL_SetSliderChangedCallback(slider, OnSlider, &progress);

    // --------------------------------------------------------
    // Button near the bottom-right corner.
    // Its right/bottom edge stays at the same relative margin.
    // --------------------------------------------------------
    HRL_id button = HRL_CreateWidget(viewport, HRL_WIDGET_BUTTON);
    HRL_SetWidgetPosition(button, 0.95f, 0.95f);
    HRL_SetWidgetSize(button, 0.25f, 0.075f);
    HRL_SetWidgetAnchor(button, 1.0f, 1.0f);
    HRL_SetButtonText(button, "Increase Progress");
    HRL_SetButtonTextSize(button, 18.0f);
    if (font != HRL_INVALID_ID)
        HRL_SetButtonTextFont(button, font);
    HRL_SetButtonPressedCallback(button, OnButton, &progress);

    // --------------------------------------------------------
    // Checkbox near the top-right corner.
    // --------------------------------------------------------
    HRL_id checkbox = HRL_CreateWidget(viewport, HRL_WIDGET_CHECKBOX);
    HRL_SetWidgetPosition(checkbox, 0.95f, 0.05f);
    HRL_SetWidgetSize(checkbox, 0.04f, 0.04f);
    HRL_SetWidgetAnchor(checkbox, 1.0f, 0.0f);
    HRL_SetCheckboxChangedCallback(checkbox, OnCheckbox, nullptr);

    // --------------------------------------------------------
    // Small labels showing how the four corner anchors behave.
    // --------------------------------------------------------
    const struct Corner {
        float x, y;
        float ax, ay;
        const char* text;
    } corners[] = {
        {0.03f, 0.04f, 0.0f, 0.0f, "Top Left"},
        {0.97f, 0.04f, 1.0f, 0.0f, "Top Right"},
        {0.03f, 0.96f, 0.0f, 1.0f, "Bottom Left"},
        {0.97f, 0.96f, 1.0f, 1.0f, "Bottom Right"},
    };

    std::vector<HRL_id> cornerLabels;
    for (const Corner& corner : corners)
    {
        HRL_id label = HRL_CreateWidget(viewport, HRL_WIDGET_LABEL);
        HRL_SetWidgetPosition(label, corner.x, corner.y);
        HRL_SetWidgetSize(label, 0.12f, 0.035f);
        HRL_SetWidgetAnchor(label, corner.ax, corner.ay);
        HRL_SetLabelText(label, corner.text);
        HRL_SetLabelTextSize(label, 14.0f);
        if (font != HRL_INVALID_ID)
            HRL_SetLabelFont(label, font);
        cornerLabels.push_back(label);
    }

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        HRL_BeginFrame();
        HRL_EndFrame();

        glfwSwapBuffers(window);
    }

    for (HRL_id id : cornerLabels)
        HRL_DeleteWidget(id);
    HRL_DeleteWidget(checkbox);
    HRL_DeleteWidget(button);
    HRL_DeleteWidget(slider);
    HRL_DeleteWidget(progress);
    HRL_DeleteWidget(title);

    HRL_DeleteViewport(viewport);
    HRL_DeleteCamera(camera);
    HRL_DeleteScene(scene);

    if (font != HRL_INVALID_ID)
        HRL_DeleteFont(font);

    HRL_Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

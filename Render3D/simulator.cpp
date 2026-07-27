#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <glm.hpp>
#include <gtc/matrix_transform.hpp>
#include <gtc/type_ptr.hpp>

#include <vector>
#include <string>
#include <iostream>
#include <random>
#include <cmath>
#include <algorithm>

// ============================= Shaders =============================

const char* vertexShaderSource = R"glsl(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in float aAlpha;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

out float lightIntensity;
out float vAlpha;

void main() {
    gl_Position = projection * view * model * vec4(aPos, 1.0);

    // Simple fake lighting (good enough for spheres)
    vec3 worldPos = (model * vec4(aPos, 1.0)).xyz;
    vec3 normal = normalize(aPos);
    vec3 dirToCenter = normalize(-worldPos);
    lightIntensity = max(dot(normal, dirToCenter), 0.15);
    vAlpha = aAlpha;
}
)glsl";

const char* fragmentShaderSource = R"glsl(
#version 330 core
in float lightIntensity;
in float vAlpha;
out vec4 FragColor;

uniform vec4 objectColor;
uniform bool isGrid;
uniform bool GLOW;
uniform bool useVertexAlpha;

void main() {
    float alpha = useVertexAlpha ? vAlpha : objectColor.a;
    if (isGrid) {
        FragColor = vec4(objectColor.rgb, alpha); // constant color for lines/arrows/grid
    } else if (GLOW) {
        FragColor = vec4(objectColor.rgb * 100000.0, objectColor.a);
    } else {
        float fade = smoothstep(0.0, 10.0, lightIntensity * 10.0);
        FragColor = vec4(objectColor.rgb * fade, objectColor.a);
    }
}
)glsl";

// ============================= Globals =============================

bool running = true;
bool paused = false;

glm::vec3 cameraPos = glm::vec3(0.0f, 250.0f, 900.0f);
glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);


float lastX = 400.0f, lastY = 300.0f;
float yaw = -90.0f;
float pitch = 0.0f;

float deltaTime = 0.0f;
float lastFrame = 0.0f;

bool isFullscreen = false;

// store windowed position + size so we can restore it
int windowedX = 100, windowedY = 100;
int windowedW = 800, windowedH = 600;

// RNG for measurement
static std::mt19937 rng{ std::random_device{}() };
static std::uniform_real_distribution<float> U01(0.0f, 1.0f);

// ============================= Forward decls =============================

GLFWwindow* StartGLU();
GLuint CreateShaderProgram(const char* vs, const char* fs);
void CreateVBOVAO(GLuint& VAO, GLuint& VBO, const float* vertices, size_t floatCount, GLenum usage = GL_STATIC_DRAW);
void UpdateCam(GLuint shaderProgram);

void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void framebuffer_size_callback(GLFWwindow* window, int width, int height);

glm::vec3 sphericalToCartesian(float r, float theta, float phi);

// ============================= Particle + Spin =============================

enum class Spin { Unknown, Up, Down };
enum class ParticleType { Electron, Proton, Positron };

static const char* particleName(ParticleType t) {
    switch (t) {
    case ParticleType::Electron: return "Electron";
    case ParticleType::Proton:   return "Proton";
    case ParticleType::Positron: return "Positron";
    }
    return "Unknown";
}

static glm::vec4 particleColor(ParticleType t) {
    // Sphere color = particle identity
    switch (t) {
    case ParticleType::Electron: return glm::vec4(0.15f, 0.65f, 1.00f, 1.0f); // blue-ish
    case ParticleType::Proton:   return glm::vec4(1.00f, 0.35f, 0.20f, 1.0f); // red/orange
    case ParticleType::Positron: return glm::vec4(0.95f, 0.25f, 0.95f, 1.0f); // magenta
    }
    return glm::vec4(1, 1, 1, 1);
}

static glm::vec4 spinArrowColor(Spin s) {
    // Arrow color = spin
    if (s == Spin::Up)   return glm::vec4(0.20f, 1.00f, 0.35f, 1.0f); // green
    if (s == Spin::Down) return glm::vec4(1.00f, 1.00f, 0.20f, 1.0f); // yellow
    return glm::vec4(1.0f, 1.0f, 1.0f, 0.35f); // unknown: faint white
}

static std::string particleNameUpper(ParticleType t) {
    switch (t) {
    case ParticleType::Electron: return "ELECTRON";
    case ParticleType::Proton:   return "PROTON";
    case ParticleType::Positron: return "POSITRON";
    }
    return "";
}

static std::string spinText(Spin s) {
    if (s == Spin::Up)   return "UP";
    if (s == Spin::Down) return "DOWN";
    return "?";
}

static Spin opposite(Spin s) {
    if (s == Spin::Up) return Spin::Down;
    if (s == Spin::Down) return Spin::Up;
    return Spin::Unknown;
}

// Entanglement state: singlet correlation (same axis => always opposite)
struct EntanglementState {
    bool entangled = true;
    bool collapsed = false; // once measured, lock spins
    Spin s1 = Spin::Unknown;
    Spin s2 = Spin::Unknown;

    void reset() {
        entangled = true;
        collapsed = false;
        s1 = Spin::Unknown;
        s2 = Spin::Unknown;
    }

    void measureParticle1(Spin result) {
        if (collapsed) return;

        if (entangled) {
            s1 = result;
            s2 = opposite(result); // singlet: opposite outcomes
            collapsed = true;
        }
        else {
            s1 = result;
            collapsed = true;
        }
    }

    void measureParticle1Random() {
        Spin r = (U01(rng) < 0.5f) ? Spin::Up : Spin::Down;
        measureParticle1(r);
    }
};

// ============================= Object (Sphere Mesh) =============================

class Object {
public:
    GLuint VAO = 0, VBO = 0;
    glm::vec3 position{ 0,0,0 };
    glm::vec4 color{ 1,1,1,1 };
    bool glow = false;

    float radius = 10.0f;
    size_t floatCount = 0;

    ParticleType type = ParticleType::Electron;

    Object(glm::vec3 pos, float r, ParticleType t) {
        position = pos;
        radius = r;
        type = t;
        color = particleColor(t);

        std::vector<float> vertices = BuildSphere();
        floatCount = vertices.size();
        CreateVBOVAO(VAO, VBO, vertices.data(), floatCount, GL_STATIC_DRAW);
    }

    void setType(ParticleType t) {
        type = t;
        color = particleColor(t);
    }

private:
    std::vector<float> BuildSphere() const {
        std::vector<float> vertices;
        int stacks = 14;
        int sectors = 14;

        for (int i = 0; i < stacks; ++i) {
            float theta1 = (float(i) / stacks) * glm::pi<float>();
            float theta2 = (float(i + 1) / stacks) * glm::pi<float>();

            for (int j = 0; j < sectors; ++j) {
                float phi1 = (float(j) / sectors) * 2.0f * glm::pi<float>();
                float phi2 = (float(j + 1) / sectors) * 2.0f * glm::pi<float>();

                glm::vec3 v1 = sphericalToCartesian(radius, theta1, phi1);
                glm::vec3 v2 = sphericalToCartesian(radius, theta1, phi2);
                glm::vec3 v3 = sphericalToCartesian(radius, theta2, phi1);
                glm::vec3 v4 = sphericalToCartesian(radius, theta2, phi2);

                // Triangle 1
                vertices.insert(vertices.end(), { v1.x, v1.y, v1.z });
                vertices.insert(vertices.end(), { v2.x, v2.y, v2.z });
                vertices.insert(vertices.end(), { v3.x, v3.y, v3.z });

                // Triangle 2
                vertices.insert(vertices.end(), { v2.x, v2.y, v2.z });
                vertices.insert(vertices.end(), { v4.x, v4.y, v4.z });
                vertices.insert(vertices.end(), { v3.x, v3.y, v3.z });
            }
        }
        return vertices;
    }
};

// ============================= Arrow Mesh (Lines) =============================
//
// A big arrow pointing along +Y in local space.
// For Spin::Down we flip it (scale Y by -1).

struct ArrowMesh {
    GLuint VAO = 0, VBO = 0;
    size_t floatCount = 0;
};

static ArrowMesh MakeArrowMesh(float shaftLen = 70.0f, float headLen = 25.0f, float headWidth = 18.0f) {
    // Arrow in local coords:
    // tail at y=0, tip at y=shaftLen
    // head wings: tip -> (±headWidth, shaftLen-headLen, 0)
    std::vector<float> v;

    // shaft
    v.insert(v.end(), { 0, 0, 0,   0, shaftLen, 0 });

    // head wings
    v.insert(v.end(), { 0, shaftLen, 0,   +headWidth, shaftLen - headLen, 0 });
    v.insert(v.end(), { 0, shaftLen, 0,   -headWidth, shaftLen - headLen, 0 });

    // extra crossbar for visibility
    v.insert(v.end(), { +headWidth * 0.8f, shaftLen - headLen, 0,   -headWidth * 0.8f, shaftLen - headLen, 0 });

    ArrowMesh a;
    a.floatCount = v.size();
    CreateVBOVAO(a.VAO, a.VBO, v.data(), v.size(), GL_STATIC_DRAW);
    return a;
}

// Draw arrow at a given world position, oriented a bit for drama (spin “twist”)
static void DrawArrow(GLuint shaderProgram, GLint modelLoc, GLint colorLoc,
    const ArrowMesh& arrow, glm::vec3 pos,
    Spin spin, float timeSeconds)
{
    glm::vec4 col = spinArrowColor(spin);

    glUniform1i(glGetUniformLocation(shaderProgram, "isGrid"), 1); // constant color lines
    glUniform1i(glGetUniformLocation(shaderProgram, "GLOW"), 0);
    glUniform4f(colorLoc, col.r, col.g, col.b, col.a);

    glm::mat4 model(1.0f);

    // Put arrow above particle
    model = glm::translate(model, pos + glm::vec3(0.0f, 25.0f, 0.0f));

    // Exaggerate: slowly rotate arrow around Y so it’s very visible (purely visual)
    model = glm::rotate(model, 0.8f * timeSeconds, glm::vec3(0, 1, 0));

    // Tilt slightly toward camera-ish direction for readability
    model = glm::rotate(model, glm::radians(20.0f), glm::vec3(1, 0, 0));

    // Up/Down: flip on Y
    if (spin == Spin::Down) {
        model = glm::scale(model, glm::vec3(1.0f, -1.0f, 1.0f));
    }

    // Unknown: shrink a bit
    if (spin == Spin::Unknown) {
        model = glm::scale(model, glm::vec3(0.8f));
    }

    glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));

    glBindVertexArray(arrow.VAO);
    glLineWidth(4.0f);
    glDrawArrays(GL_LINES, 0, (GLsizei)(arrow.floatCount / 3));
    glBindVertexArray(0);
}

// ============================= Path motion =============================
//
// Exaggerated, readable motion:
// - big "figure-8" / Lissajous-ish paths
// - lots of vertical bobbing

static glm::vec3 pathParticle1(float t) {
    float A = 260.0f;
    float B = 170.0f;
    float C = 110.0f;

    float x = A * std::sin(1.0f * t);
    float z = B * std::sin(2.0f * t + 0.9f);
    float y = 90.0f + C * std::sin(1.7f * t + 0.3f);
    return glm::vec3(x, y, z);
}

static glm::vec3 pathParticle2(float t) {
    // Mirror-ish but not identical (still looks "paired")
    float A = 260.0f;
    float B = 170.0f;
    float C = 110.0f;

    float x = -A * std::sin(1.0f * t + 0.4f);
    float z = -B * std::sin(2.0f * t + 0.9f);
    float y = 90.0f + C * std::sin(1.7f * t + 0.3f + 3.14159f);
    return glm::vec3(x, y, z);
}


void ToggleFullscreen(GLFWwindow* window) {
    isFullscreen = !isFullscreen;

    if (isFullscreen) {
        // save current windowed position/size
        glfwGetWindowPos(window, &windowedX, &windowedY);
        glfwGetWindowSize(window, &windowedW, &windowedH);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);

        // switch to fullscreen at native resolution + refresh rate
        glfwSetWindowMonitor(
            window,
            monitor,
            0, 0,
            mode->width,
            mode->height,
            mode->refreshRate
        );
    }
    else {
        // restore windowed mode
        glfwSetWindowMonitor(
            window,
            nullptr,
            windowedX, windowedY,
            windowedW, windowedH,
            0
        );
    }
}

struct Trail {
    GLuint VAO = 0, VBO = 0;
    std::vector<glm::vec4> points; // xyz = position, w = alpha
    int maxPoints = 600;

    void init(int maxP) {
        maxPoints = maxP;
        points.reserve(maxPoints);

        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);

        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, maxPoints * sizeof(glm::vec4), nullptr, GL_DYNAMIC_DRAW);

        // position: xyz at location 0
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), (void*)0);
        glEnableVertexAttribArray(0);

        // alpha: w at location 1
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);

        glBindVertexArray(0);
    }

    void addPoint(const glm::vec3& p) {
        if ((int)points.size() < maxPoints) {
            points.push_back(glm::vec4(p, 0.0f));
        }
        else {
            points.erase(points.begin());
            points.push_back(glm::vec4(p, 0.0f));
        }

        // Recompute fade: oldest = near 0 alpha, newest = 1.0
        int n = (int)points.size();
        for (int i = 0; i < n; i++) {
            points[i].w = (float)(i + 1) / (float)n;
        }

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, points.size() * sizeof(glm::vec4), points.data());
    }

    void draw() const {
        glBindVertexArray(VAO);
        glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)points.size());
        glBindVertexArray(0);
    }

    void cleanup() {
        glDeleteVertexArrays(1, &VAO);
        glDeleteBuffers(1, &VBO);
    }
};

// ============================= Stroke Text & Popups =============================
//
// Each glyph: flat vector of segments as {x1,y1, x2,y2} pairs, coords in [0,1]²
// y=0 is bottom, y=1 is top.

static std::vector<float> getGlyph(char c) {
    switch (c) {
    case ' ': return {};
    case 'C': return {1,.75f, .6f,1,  .6f,1, 0,1,  0,1, 0,0,  0,0, .6f,0,  .6f,0, 1,.25f};
    case 'D': return {0,0, 0,1,  0,1, .65f,1,  .65f,1, 1,.75f,  1,.75f, 1,.25f,  1,.25f, .65f,0,  .65f,0, 0,0};
    case 'E': return {0,0, 0,1,  0,1, 1,1,  0,.5f, .8f,.5f,  0,0, 1,0};
    case 'I': return {0,1, 1,1,  .5f,1, .5f,0,  0,0, 1,0};
    case 'L': return {0,1, 0,0,  0,0, 1,0};
    case 'N': return {0,0, 0,1,  0,1, 1,0,  1,0, 1,1};
    case 'O': return {0,0, 0,1,  0,1, 1,1,  1,1, 1,0,  1,0, 0,0};
    case 'P': return {0,0, 0,1,  0,1, 1,1,  1,1, 1,.5f,  1,.5f, 0,.5f};
    case 'R': return {0,0, 0,1,  0,1, 1,1,  1,1, 1,.5f,  1,.5f, 0,.5f,  0,.5f, 1,0};
    case 'S': return {1,1, 0,1,  0,1, 0,.5f,  0,.5f, 1,.5f,  1,.5f, 1,0,  1,0, 0,0};
    case 'T': return {0,1, 1,1,  .5f,1, .5f,0};
    case 'U': return {0,1, 0,0,  0,0, 1,0,  1,0, 1,1};
    case 'W': return {0,1, .25f,0,  .25f,0, .5f,.5f,  .5f,.5f, .75f,0,  .75f,0, 1,1};
    case '?': return {.25f,1, .75f,1,  .75f,1, 1,.75f,  1,.75f, .5f,.4f,  .5f,.4f, .5f,.2f,  .4f,.07f, .6f,.07f};
    default:  return {};
    }
}

// Build GL_LINES vertex array for text in NDC space.
// x,y = bottom-left origin; cw = char width, ch = char height, sp = gap between chars.
static std::vector<float> buildTextVerts(const std::string& text, float x, float y,
                                          float cw, float ch, float sp) {
    std::vector<float> v;
    float cx = x;
    for (char c : text) {
        auto segs = getGlyph(c);
        for (size_t i = 0; i + 4 <= segs.size(); i += 4) {
            v.push_back(cx + segs[i]     * cw); v.push_back(y + segs[i + 1] * ch); v.push_back(0.0f);
            v.push_back(cx + segs[i + 2] * cw); v.push_back(y + segs[i + 3] * ch); v.push_back(0.0f);
        }
        cx += cw + sp;
    }
    return v;
}

struct Popup {
    std::string text;
    float x = 0, y = 0;       // NDC bottom-left of text
    float timer = 0;
    float duration = 2.5f;
    glm::vec3 color{ 1,1,1 };
    bool active = false;

    void trigger(const std::string& t, float px, float py, glm::vec3 col, float dur = 2.5f) {
        text = t; x = px; y = py; color = col; timer = dur; duration = dur; active = true;
    }

    float alpha() const {
        if (!active) return 0.0f;
        const float fadeTime = 0.5f;
        return std::min(1.0f, timer / fadeTime);
    }

    void update(float dt) {
        if (!active) return;
        timer -= dt;
        if (timer <= 0.0f) { timer = 0.0f; active = false; }
    }
};

struct TextRenderer {
    GLuint VAO = 0, VBO = 0;
    static constexpr int MAX_FLOATS = 4000;

    void init() {
        glGenVertexArrays(1, &VAO);
        glGenBuffers(1, &VBO);
        glBindVertexArray(VAO);
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferData(GL_ARRAY_BUFFER, MAX_FLOATS * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    void drawVerts(const std::vector<float>& verts) const {
        if (verts.empty()) return;
        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, verts.size() * sizeof(float), verts.data());
        glBindVertexArray(VAO);
        glLineWidth(2.0f);
        glDrawArrays(GL_LINES, 0, (GLsizei)(verts.size() / 3));
        glBindVertexArray(0);
    }

    void cleanup() {
        glDeleteVertexArrays(1, &VAO);
        glDeleteBuffers(1, &VBO);
    }
};

// ============================= Main =============================

int main() {
    GLFWwindow* window = StartGLU();
    if (!window) return -1;

    GLuint shaderProgram = CreateShaderProgram(vertexShaderSource, fragmentShaderSource);
    glUseProgram(shaderProgram);

    // camera callbacks
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetKeyCallback(window, keyCallback);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    // projection
    glm::mat4 projection = glm::perspective(glm::radians(45.0f), 800.0f / 600.0f, 0.1f, 50000.0f);
    GLint projectionLoc = glGetUniformLocation(shaderProgram, "projection");
    glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, glm::value_ptr(projection));

    GLint modelLoc = glGetUniformLocation(shaderProgram, "model");
    GLint objectColorLoc = glGetUniformLocation(shaderProgram, "objectColor");

    // two particles
    std::vector<Object> objs;
    objs.emplace_back(glm::vec3(-200, 120, 0), 18.0f, ParticleType::Electron);
    objs.emplace_back(glm::vec3(+200, 120, 0), 18.0f, ParticleType::Proton);

    Trail trail1, trail2;
    trail1.init(900);
    trail2.init(900);

    // Popups: P1/P2 show particle type; S1/S2 show spin state
    Popup popupP1, popupP2, popupS1, popupS2;
    TextRenderer textRenderer;
    textRenderer.init();

    ArrowMesh arrow = MakeArrowMesh();

    // entanglement (singlet)
    EntanglementState ent;
    ent.reset();

    // Print controls once
    std::cout << "\n=== Entanglement Demo Controls ===\n";
    std::cout << "Camera: WASD + Space/Shift, Mouse look, Scroll\n";
    std::cout << "P: pause/resume motion\n";
    std::cout << "R: reset entanglement (spins unknown)\n";
    std::cout << "M: measure particle 1 randomly (Up/Down)\n";
    std::cout << "U: force particle 1 Up (demo)\n";
    std::cout << "J: force particle 1 Down (demo)\n";
    std::cout << "1/2/3: set particle 1 type (1=electron,2=proton,3=positron)\n";
    std::cout << "Shift+1/2/3: set particle 2 type\n";
    std::cout << "Esc: quit\n\n";

    while (!glfwWindowShouldClose(window) && running) {
        float currentFrame = (float)glfwGetTime();
        deltaTime = currentFrame - lastFrame;
        lastFrame = currentFrame;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Update camera
        UpdateCam(shaderProgram);

        // Aspect Ratio
        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        glm::mat4 projection = glm::perspective(glm::radians(45.0f), (float)fbW / (float)fbH, 0.1f, 50000.0f);
        GLint projectionLoc = glGetUniformLocation(shaderProgram, "projection");
        glUniformMatrix4fv(projectionLoc, 1, GL_FALSE, glm::value_ptr(projection));

        // Motion
        float t = currentFrame;
        if (!paused) {
            objs[0].position = pathParticle1(t);
            objs[1].position = pathParticle2(t);

            trail1.addPoint(objs[0].position);
            trail2.addPoint(objs[1].position);
        }

        // Visual glow when collapsed (so entanglement "event" pops)
        objs[0].glow = ent.collapsed;
        objs[1].glow = ent.collapsed;

        // Draw spheres
        for (int i = 0; i < (int)objs.size(); ++i) {
            Object& obj = objs[i];

            // sphere color = particle type
            obj.color = particleColor(obj.type);

            glUseProgram(shaderProgram);
            glUniform1i(glGetUniformLocation(shaderProgram, "isGrid"), 0);
            glUniform1i(glGetUniformLocation(shaderProgram, "GLOW"), obj.glow ? 1 : 0);
            glUniform4f(objectColorLoc, obj.color.r, obj.color.g, obj.color.b, obj.color.a);

            glm::mat4 model(1.0f);
            model = glm::translate(model, obj.position);
            glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));

            glBindVertexArray(obj.VAO);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(obj.floatCount / 3));
        }

        // Draw trails with fade (transparent at tail, opaque at head)
        glUseProgram(shaderProgram);
        glUniform1i(glGetUniformLocation(shaderProgram, "isGrid"), 1);
        glUniform1i(glGetUniformLocation(shaderProgram, "GLOW"), 0);
        glUniform1i(glGetUniformLocation(shaderProgram, "useVertexAlpha"), 1);

        glm::vec4 c1 = particleColor(objs[0].type);
        glUniform4f(objectColorLoc, c1.r, c1.g, c1.b, 1.0f);
        glLineWidth(3.0f);
        trail1.draw();

        glm::vec4 c2 = particleColor(objs[1].type);
        glUniform4f(objectColorLoc, c2.r, c2.g, c2.b, 1.0f);
        glLineWidth(3.0f);
        trail2.draw();

        glUniform1i(glGetUniformLocation(shaderProgram, "useVertexAlpha"), 0);

        // Draw spin arrows (arrow color shows spin)
        DrawArrow(shaderProgram, modelLoc, objectColorLoc, arrow, objs[0].position, ent.s1, t);
        DrawArrow(shaderProgram, modelLoc, objectColorLoc, arrow, objs[1].position, ent.s2, t);

        // Draw text popups (2D overlay in NDC space)
        {
            glm::mat4 ident(1.0f);
            glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "view"),       1, GL_FALSE, glm::value_ptr(ident));
            glUniformMatrix4fv(projectionLoc,                                      1, GL_FALSE, glm::value_ptr(ident));
            glUniformMatrix4fv(modelLoc,                                           1, GL_FALSE, glm::value_ptr(ident));
            glUniform1i(glGetUniformLocation(shaderProgram, "isGrid"),        1);
            glUniform1i(glGetUniformLocation(shaderProgram, "GLOW"),          0);
            glUniform1i(glGetUniformLocation(shaderProgram, "useVertexAlpha"),0);
            glDisable(GL_DEPTH_TEST);

            const float CW = 0.050f, CH = 0.080f, SP = 0.012f;
            Popup* ps[] = { &popupP1, &popupP2, &popupS1, &popupS2 };
            for (Popup* p : ps) {
                p->update(deltaTime);
                float a = p->alpha();
                if (a <= 0.0f) continue;
                glUniform4f(objectColorLoc, p->color.r, p->color.g, p->color.b, a);
                textRenderer.drawVerts(buildTextVerts(p->text, p->x, p->y, CW, CH, SP));
            }

            glEnable(GL_DEPTH_TEST);
        }

        glfwSwapBuffers(window);
        glfwPollEvents();

        static bool f11Down = false;
        if (glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS) {
            if (!f11Down) {
                ToggleFullscreen(window);
                f11Down = true;
            }
        }
        else {
            f11Down = false;
        }

        // ---- Handle entanglement input actions via GLFW key polling ----
        // (We do it here so we can access 'ent' and 'objs' without global pointers)
        // But we still also use keyCallback for camera movement.
        {
            static bool pDown = false;
            if (glfwGetKey(window, GLFW_KEY_P) == GLFW_PRESS) {
                if (!pDown) { paused = !paused; pDown = true; }
            }
            else {
                pDown = false;
            }
        }

        // Helper: trigger spin popups for both particles
        auto triggerSpinPopups = [&]() {
            auto col = [](Spin s) -> glm::vec3 { glm::vec4 c = spinArrowColor(s); return glm::vec3(c.r, c.g, c.b); };
            popupS1.trigger(spinText(ent.s1), -0.95f, -0.77f, col(ent.s1));
            popupS2.trigger(spinText(ent.s2),  0.25f, -0.77f, col(ent.s2));
        };

        // Measurement triggers
        static bool mDown = false, uDown = false, jDown = false, rDown = false;
        if (glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS) {
            if (!mDown) { ent.measureParticle1Random(); mDown = true; triggerSpinPopups(); }
        }
        else mDown = false;

        if (glfwGetKey(window, GLFW_KEY_U) == GLFW_PRESS) {
            if (!uDown) { ent.measureParticle1(Spin::Up); uDown = true; triggerSpinPopups(); }
        }
        else uDown = false;

        if (glfwGetKey(window, GLFW_KEY_J) == GLFW_PRESS) {
            if (!jDown) { ent.measureParticle1(Spin::Down); jDown = true; triggerSpinPopups(); }
        }
        else jDown = false;

        if (glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS) {
            if (!rDown) { ent.reset(); rDown = true; triggerSpinPopups(); }
        }
        else rDown = false;

        // Particle type selection: 1/2/3 for particle 1, Shift+1/2/3 for particle 2
        bool shiftHeld = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ||
            (glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);

        auto typeFromKey = [&](int key)->ParticleType {
            if (key == GLFW_KEY_1) return ParticleType::Electron;
            if (key == GLFW_KEY_2) return ParticleType::Proton;
            return ParticleType::Positron;
            };

        static bool oneDown = false, twoDown = false, threeDown = false;
        auto handleTypeKey = [&](int key, bool& latch) {
            if (glfwGetKey(window, key) == GLFW_PRESS) {
                if (!latch) {
                    ParticleType tsel = typeFromKey(key);
                    glm::vec4 tc = particleColor(tsel);
                    glm::vec3 tcol(tc.r, tc.g, tc.b);
                    if (!shiftHeld) {
                        objs[0].setType(tsel);
                        popupP1.trigger(particleNameUpper(tsel), -0.95f, -0.62f, tcol);
                    }
                    else {
                        objs[1].setType(tsel);
                        popupP2.trigger(particleNameUpper(tsel),  0.25f, -0.62f, tcol);
                    }
                    std::cout << (shiftHeld ? "Particle 2 -> " : "Particle 1 -> ")
                        << particleName(tsel) << "\n";
                    latch = true;
                }
            }
            else {
                latch = false;
            }
            };

        handleTypeKey(GLFW_KEY_1, oneDown);
        handleTypeKey(GLFW_KEY_2, twoDown);
        handleTypeKey(GLFW_KEY_3, threeDown);
    }

    // cleanup
    for (auto& obj : objs) {
        glDeleteVertexArrays(1, &obj.VAO);
        glDeleteBuffers(1, &obj.VBO);
    }
    glDeleteVertexArrays(1, &arrow.VAO);
    glDeleteBuffers(1, &arrow.VBO);

    glDeleteProgram(shaderProgram);
    trail1.cleanup();
    trail2.cleanup();
    textRenderer.cleanup();

    glfwTerminate();
    return 0;
}

// ============================= OpenGL Helpers =============================

GLFWwindow* StartGLU() {
    if (!glfwInit()) {
        std::cout << "Failed to initialize GLFW\n";
        return nullptr;
    }

    // OpenGL 3.3 core
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(800, 600, "Entanglement Spin Demo", NULL, NULL);
    if (!window) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return nullptr;
    }
    glfwMakeContextCurrent(window);

    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) {
        std::cerr << "Failed to initialize GLEW\n";
        glfwTerminate();
        return nullptr;
    }

    glEnable(GL_DEPTH_TEST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, 800, 600);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    return window;
}

GLuint CreateShaderProgram(const char* vs, const char* fs) {
    GLuint v = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(v, 1, &vs, nullptr);
    glCompileShader(v);

    GLint ok = 0;
    glGetShaderiv(v, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(v, 1024, nullptr, log);
        std::cerr << "Vertex shader compile error:\n" << log << "\n";
    }

    GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(f, 1, &fs, nullptr);
    glCompileShader(f);

    glGetShaderiv(f, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(f, 1024, nullptr, log);
        std::cerr << "Fragment shader compile error:\n" << log << "\n";
    }

    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);

    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, 1024, nullptr, log);
        std::cerr << "Shader program link error:\n" << log << "\n";
    }

    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

void CreateVBOVAO(GLuint& VAO, GLuint& VBO, const float* vertices, size_t floatCount, GLenum usage) {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(VAO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, floatCount * sizeof(float), vertices, usage);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);

    glBindVertexArray(0);
}

void UpdateCam(GLuint shaderProgram) {
    glUseProgram(shaderProgram);
    glm::mat4 view = glm::lookAt(cameraPos, cameraPos + cameraFront, cameraUp);
    GLint viewLoc = glGetUniformLocation(shaderProgram, "view");
    glUniformMatrix4fv(viewLoc, 1, GL_FALSE, glm::value_ptr(view));
}

glm::vec3 sphericalToCartesian(float r, float theta, float phi) {
    float x = r * std::sin(theta) * std::cos(phi);
    float y = r * std::cos(theta);
    float z = r * std::sin(theta) * std::sin(phi);
    return glm::vec3(x, y, z);
}

// ============================= Input: camera only =============================

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    (void)window;
    glViewport(0, 0, width, height);
}

void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)key; (void)scancode; (void)mods; (void)action;

    // ESC quit
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        running = false;
        glfwSetWindowShouldClose(window, GLFW_TRUE);
        return;
    }

    // Camera movement
    float cameraSpeed = 600.0f * deltaTime;

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cameraPos += cameraSpeed * cameraFront;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cameraPos -= cameraSpeed * cameraFront;

    glm::vec3 right = glm::normalize(glm::cross(cameraFront, cameraUp));
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) cameraPos -= cameraSpeed * right;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) cameraPos += cameraSpeed * right;

    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)      cameraPos += cameraSpeed * cameraUp;
    if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) cameraPos -= cameraSpeed * cameraUp;
}

void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    (void)window;

    float xoffset = (float)xpos - lastX;
    float yoffset = lastY - (float)ypos;
    lastX = (float)xpos;
    lastY = (float)ypos;

    float sensitivity = 0.10f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    yaw += xoffset;
    pitch += yoffset;

    pitch = std::clamp(pitch, -89.0f, 89.0f);

    glm::vec3 front;
    front.x = std::cos(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    front.y = std::sin(glm::radians(pitch));
    front.z = std::sin(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    cameraFront = glm::normalize(front);
}

void scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
    (void)window; (void)xoffset;
    float cameraSpeed = 2200.0f * deltaTime;
    if (yoffset > 0) cameraPos += cameraSpeed * cameraFront;
    if (yoffset < 0) cameraPos -= cameraSpeed * cameraFront;
}
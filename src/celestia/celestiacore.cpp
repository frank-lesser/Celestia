// celestiacore.cpp
//
// Platform-independent UI handling and initialization for Celestia.
// winmain, gtkmain, and glutmain are thin, platform-specific modules
// that sit directly on top of CelestiaCore and feed it mouse and
// keyboard events.  CelestiaCore then turns those events into calls
// to Renderer and Simulation.
//
// Copyright (C) 2001-2009, the Celestia Development Team
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#include "celestiacore.h"
#include "favorites.h"
#include "url.h"
#include <celengine/astro.h>
#include <celengine/asterism.h>
#include <celengine/boundaries.h>
#include <celengine/overlay.h>
#include <celengine/console.h>
#include <celengine/execution.h>
#include <celengine/cmdparser.h>
#include <celengine/multitexture.h>
#ifdef USE_SPICE
#include <celephem/spiceinterface.h>
#endif
#include <celengine/axisarrow.h>
#include <celengine/planetgrid.h>
#include <celengine/visibleregion.h>
#include <celmath/geomutil.h>
#include <celutil/util.h>
#include <celutil/filetype.h>
#include <celutil/directory.h>
#include <celutil/formatnum.h>
#include <celutil/debug.h>
#include <celutil/utf8.h>
#include <Eigen/Geometry>
#include <GL/glew.h>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cassert>
#include <ctime>
#include <fmt/printf.h>

#ifdef CELX
#include <celephem/scriptobject.h>
#endif

// TODO: proper gettext
#define C_(a, b) (b)


using namespace Eigen;
using namespace std;
using namespace celmath;

static const int DragThreshold = 3;

// Perhaps you'll want to put this stuff in configuration file.
static const double CoarseTimeScaleFactor = 10.0;
static const double FineTimeScaleFactor = 2.0;
static const double fMaxKeyAccel = 20.0;
static const float RotationBraking = 10.0f;
static const float RotationDecay = 2.0f;
static const double MaximumTimeRate = 1.0e15;
static const double MinimumTimeRate = 1.0e-15;
static const float stdFOV = degToRad(45.0f);
static const float MaximumFOV = degToRad(120.0f);
static const float MinimumFOV = degToRad(0.001f);
static float KeyRotationAccel = degToRad(120.0f);
static float MouseRotationSensitivity = degToRad(1.0f);

static const int ConsolePageRows = 10;
static Console console(200, 120);

static void warning(string s)
{
    cout << s;
}


// Extremely basic implementation of an ExecutionEnvironment for
// running scripts.
class CoreExecutionEnvironment : public ExecutionEnvironment
{
private:
    CelestiaCore& core;

public:
    CoreExecutionEnvironment(CelestiaCore& _core) : core(_core)
    {
    }

    Simulation* getSimulation() const
    {
        return core.getSimulation();
    }

    Renderer* getRenderer() const
    {
        return core.getRenderer();
    }

    CelestiaCore* getCelestiaCore() const
    {
        return &core;
    }

    void showText(string s, int horig, int vorig, int hoff, int voff,
                  double duration)
    {
        core.showText(s, horig, vorig, hoff, voff, duration);
    }
};


// If right dragging to rotate, adjust the rotation rate based on the
// distance from the reference object.  This makes right drag rotation
// useful even when the camera is very near the surface of an object.
// Disable adjustments if the reference is a deep sky object, since they
// have no true surface (and the observer is likely to be inside one.)
float ComputeRotationCoarseness(Simulation& sim)
{
    float coarseness = 1.5f;

    Selection selection = sim.getActiveObserver()->getFrame()->getRefObject();
    if (selection.getType() == Selection::Type_Star ||
        selection.getType() == Selection::Type_Body)
    {
        double radius = selection.radius();
        double t = sim.getTime();
        UniversalCoord observerPosition = sim.getActiveObserver()->getPosition();
        UniversalCoord selectionPosition = selection.getPosition(t);
        double distance = observerPosition.distanceFromKm(selectionPosition);
        double altitude = distance - radius;
        if (altitude > 0.0 && altitude < radius)
        {
            coarseness *= (float) max(0.01, altitude / radius);
        }
    }

    return coarseness;
}


View::View(View::Type _type,
           Observer* _observer,
           float _x, float _y,
           float _width, float _height) :
    type(_type),
    observer(_observer),
    parent(nullptr),
    child1(nullptr),
    child2(nullptr),
    x(_x),
    y(_y),
    width(_width),
    height(_height),
    renderFlags(0),
    labelMode(0),
    zoom(1),
    alternateZoom(1)
{
}

void View::mapWindowToView(float wx, float wy, float& vx, float& vy) const
{
    vx = (wx - x) / width;
    vy = (wy + (y + height - 1)) / height;
    vx = (vx - 0.5f) * (width / height);
    vy = 0.5f - vy;
}

void View::walkTreeResize(View* sibling, int sign) {
   float ratio;
   switch (parent->type)
    {
    case View::HorizontalSplit:
        ratio = parent->height / (parent->height -  height);
        sibling->height *= ratio;
        if (sign == 1)
        {
            sibling->y = parent->y + (sibling->y - parent->y) * ratio;
        }
        else
        {
            sibling->y = parent->y + (sibling->y - (y + height)) * ratio;
        }
        break;

    case View::VerticalSplit:
        ratio = parent->width / (parent->width - width);
        sibling->width *= ratio;
        if (sign == 1)
        {
            sibling->x = parent->x + (sibling->x - parent->x) * ratio;
        }
        else
        {
            sibling->x = parent->x + (sibling->x - (x + width) ) * ratio;
        }
        break;
    case View::ViewWindow:
        break;
    }
    if (sibling->child1) walkTreeResize(sibling->child1, sign);
    if (sibling->child2) walkTreeResize(sibling->child2, sign);
}

bool View::walkTreeResizeDelta(View* v, float delta, bool check)
{
   View *p=v;
   int sign = -1;
   float ratio;
   double newSize;

   if (v->child1)
   {
       if (!walkTreeResizeDelta(v->child1, delta, check))
           return false;
   }

   if (v->child2)
   {
       if (!walkTreeResizeDelta(v->child2, delta, check))
           return false;
   }

   while ( p != child1 && p != child2 && (p = p->parent) ) ;
   if (p == child1) sign = 1;
   switch (type)
    {
    case View::HorizontalSplit:
        delta = -delta;
        ratio = (p->height  + sign * delta) / p->height;
        newSize = v->height * ratio;
        if (newSize <= .1) return false;
        if (check) return true;
        v->height = (float) newSize;
        if (sign == 1)
        {
            v->y = p->y + (v->y - p->y) * ratio;
        }
        else
        {
            v->y = p->y + delta + (v->y - p->y) * ratio;
        }
        break;

    case View::VerticalSplit:
        ratio = (p->width + sign * delta) / p->width;
        newSize = v->width * ratio;
        if (newSize <= .1) return false;
        if (check) return true;
        v->width = (float) newSize;
        if (sign == 1)
        {
            v->x = p->x + (v->x - p->x) * ratio;
        }
        else
        {
            v->x = p->x + delta + (v->x - p->x) * ratio;
        }
        break;
    case View::ViewWindow:
        break;
    }

    return true;
}


CelestiaCore::CelestiaCore() :
    oldFOV(stdFOV)
{
    /* Get a renderer here so it may be queried for capabilities of the
       underlying engine even before rendering is enabled. It's initRenderer()
       routine will be called much later. */
    renderer = new Renderer();
    timer = new Timer();

    execEnv = new CoreExecutionEnvironment(*this);

    for (int i = 0; i < KeyCount; i++)
    {
        keysPressed[i] = false;
        shiftKeysPressed[i] = false;
    }
    for (int i = 0; i < JoyButtonCount; i++)
        joyButtonsPressed[i] = false;

    clog.rdbuf(console.rdbuf());
    cerr.rdbuf(console.rdbuf());
    console.setWindowHeight(ConsolePageRows);
}

CelestiaCore::~CelestiaCore()
{
    if (movieCapture != nullptr)
        recordEnd();

#ifdef CELX
    // Clean up all scripts
    delete celxScript;
    delete luaHook;
    delete luaSandbox;
#endif

    delete execEnv;
    delete timer;
    delete renderer;
}

void CelestiaCore::readFavoritesFile()
{
    // Set up favorites list
    if (config->favoritesFile != "")
    {
        ifstream in(config->favoritesFile, ios::in);

        if (in.good())
        {
            favorites = ReadFavoritesList(in);
            if (favorites == nullptr)
            {
                warning(_("Error reading favorites file."));
            }
        }
    }
}

void CelestiaCore::writeFavoritesFile()
{
    if (config->favoritesFile != "")
    {
        ofstream out(config->favoritesFile, ios::out);
        if (out.good())
            WriteFavoritesList(*favorites, out);
    }
}

void CelestiaCore::activateFavorite(FavoritesEntry& fav)
{
    sim->cancelMotion();
    sim->setTime(fav.jd);
    sim->setObserverPosition(fav.position);
    sim->setObserverOrientation(fav.orientation);
    sim->setSelection(sim->findObjectFromPath(fav.selectionName));
    sim->setFrame(fav.coordSys, sim->getSelection());
}

void CelestiaCore::addFavorite(string name, string parentFolder, FavoritesList::iterator* iter)
{
    FavoritesList::iterator pos;
    if(!iter)
        pos = favorites->end();
    else
        pos = *iter;
    auto* fav = new FavoritesEntry();
    fav->jd = sim->getTime();
    fav->position = sim->getObserver().getPosition();
    fav->orientation = sim->getObserver().getOrientationf();
    fav->name = name;
    fav->isFolder = false;
    fav->parentFolder = parentFolder;

    Selection sel = sim->getSelection();
    if (sel.deepsky() != nullptr)
        fav->selectionName = sim->getUniverse()->getDSOCatalog()->getDSOName(sel.deepsky());
    else
        fav->selectionName = sel.getName();

    fav->coordSys = sim->getFrame()->getCoordinateSystem();

    favorites->insert(pos, fav);
}

void CelestiaCore::addFavoriteFolder(string name, FavoritesList::iterator* iter)
{
    FavoritesList::iterator pos;
    if(!iter)
        pos = favorites->end();
    else
        pos = *iter;
    auto* fav = new FavoritesEntry();
    fav->name = name;
    fav->isFolder = true;

    favorites->insert(pos, fav);
}

FavoritesList* CelestiaCore::getFavorites()
{
    return favorites;
}


const DestinationList* CelestiaCore::getDestinations()
{
    return destinations;
}


// Used in the super-secret edit mode
void showSelectionInfo(const Selection& sel)
{
    Quaternionf orientation;
    if (sel.deepsky() != nullptr)
        orientation = sel.deepsky()->getOrientation();
    else if (sel.body() != nullptr)
        orientation = sel.body()->getGeometryOrientation();

    AngleAxisf aa(orientation);

    fmt::printf(_("%s\nOrientation: [%f, %f, %f], %.1f\n"),
                sel.getName(), aa.axis().x(), aa.axis().y(), aa.axis().z(), radToDeg(aa.angle()));
}


void CelestiaCore::cancelScript()
{
    if (runningScript != nullptr)
    {
        delete runningScript;
        scriptState = ScriptCompleted;
        runningScript = nullptr;
    }
#ifdef CELX
    if (celxScript != nullptr)
    {
        celxScript->cleanup();
        if (textEnterMode & KbPassToScript)
            setTextEnterMode(textEnterMode & ~KbPassToScript);
        scriptState = ScriptCompleted;
    }
#endif
}


void CelestiaCore::runScript(CommandSequence* script)
{
    cancelScript();
    if (runningScript == nullptr && script != nullptr && scriptState == ScriptCompleted)
    {
        scriptState = ScriptRunning;
        runningScript = new Execution(*script, *execEnv);
    }
}


void CelestiaCore::runScript(const string& filename)
{
    cancelScript();
    string localeFilename = LocaleFilename(filename);
    ContentType type = DetermineFileType(localeFilename);

    if (type == Content_CelestiaLegacyScript)
    {
        ifstream scriptfile(localeFilename);
        if (!scriptfile.good())
        {
            fatalError(_("Error opening script file."));
        }
        else
        {
            CommandParser parser(scriptfile);
            CommandSequence* script = parser.parse();
            if (script == nullptr)
            {
                const vector<string>* errors = parser.getErrors();
                string errorMsg;
                if (errors->size() > 0)
                    errorMsg = (*errors)[0];
                fatalError(errorMsg);
            }
            else
            {
                runningScript = new Execution(*script, *execEnv);
                scriptState = sim->getPauseState()?ScriptPaused:ScriptRunning;
            }
        }
    }
#ifdef CELX
    else if (type == Content_CelestiaScript)
    {
        ifstream scriptfile(localeFilename);
        if (!scriptfile.good())
        {
            string errMsg;
            errMsg = fmt::sprintf(_("Error opening script '%s'"), localeFilename);
            fatalError(errMsg);
        }

        if (celxScript == nullptr)
        {
            celxScript = new LuaState();
            celxScript->init(this);
        }

        int status = celxScript->loadScript(scriptfile, localeFilename);
        if (status != 0)
        {
            string errMsg = celxScript->getErrorMessage();
            if (errMsg.empty())
                errMsg = _("Unknown error opening script");
            fatalError(errMsg);
        }
        else
        {
            // Coroutine execution; control may be transferred between the
            // script and Celestia's event loop
            if (!celxScript->createThread())
            {
                fatalError(_("Script coroutine initialization failed"));
            }
            else
            {
                scriptState = sim->getPauseState()?ScriptPaused:ScriptRunning;
            }
        }
    }
#endif
    else
    {
        fatalError(_("Invalid filetype"));
    }
}


bool checkMask(int modifiers, int mask)
{
    return (modifiers & mask) == mask;
}

void CelestiaCore::mouseButtonDown(float x, float y, int button)
{
    setViewChanged();

    mouseMotion = 0.0f;

#ifdef CELX
    if (celxScript != nullptr)
    {
        if (celxScript->handleMouseButtonEvent(x, y, button, true))
            return;
    }

    if (luaHook && luaHook->callLuaHook(this, "mousebuttondown", x, y, button))
        return;
#endif

    if (views.size() > 1)
    {
        // To select the clicked into view before a drag.
        pickView(x, y);
    }

    if (views.size() > 1 && button == LeftButton) // look if click is near a view border
    {
        View *v1 = nullptr, *v2 = nullptr;
        for (const auto v : views)
        {
            if (v->type == View::ViewWindow)
            {
                float vx, vy, vxp, vyp;
                vx = ( x / width - v->x ) / v->width;
                vy = ( (1 - y / height ) - v->y ) / v->height;
                vxp = vx * v->width * width;
                vyp = vy * v->height * height;
                if ( (vx >=0 && vx <= 1 && ( abs(vyp) <= 2 || abs(vyp - v->height * height) <= 2))
                  || (vy >=0 && vy <= 1 && ( abs(vxp) <= 2 || abs(vxp - v->width * width) <= 2)) )
                {
                    if (v1 == 0)
                    {
                        v1 = v;
                    }
                    else
                    {
                        v2 = v;
                        break;
                    }
                }
            }
        }
        if (v2 != nullptr) {
             // Look for common ancestor to v1 & v2 = split being draged.
             View *p1 = v1, *p2 = v2;
             while ( (p1 = p1->parent) != nullptr )
             {
                 p2 = v2;
                 while ( ((p2 = p2->parent) != nullptr) && p1 != p2) ;
                 if (p2 != nullptr) break;
             }
             if (p2 != nullptr)
             {
                 resizeSplit = p1;
             }
        }
    }

}

void CelestiaCore::mouseButtonUp(float x, float y, int button)
{
    setViewChanged();

    // Four pixel tolerance for picking
    float pickTolerance = sim->getActiveObserver()->getFOV() / height * 4.0f;

    if (resizeSplit != nullptr)
    {
        resizeSplit = nullptr;
        return;
    }

#ifdef CELX
    if (celxScript != nullptr)
    {
        if (celxScript->handleMouseButtonEvent(x, y, button, false))
            return;
    }

    if (luaHook && luaHook->callLuaHook(this,"mousebuttonup", x, y, button))
        return;
#endif

    // If the mouse hasn't moved much since it was pressed, treat this
    // as a selection or context menu event.  Otherwise, assume that the
    // mouse was dragged and ignore the event.
    if (mouseMotion < DragThreshold)
    {
        if (button == LeftButton)
        {
            pickView(x, y);

            float pickX, pickY;
            float aspectRatio = ((float) width / (float) height);
            (*activeView)->mapWindowToView((float) x / (float) width,
                                        (float) y / (float) height,
                                        pickX, pickY);
            Vector3f pickRay =
                sim->getActiveObserver()->getPickRay(pickX * aspectRatio, pickY);

            Selection oldSel = sim->getSelection();
            Selection newSel = sim->pickObject(pickRay, renderer->getRenderFlags(), pickTolerance);
            addToHistory();
            sim->setSelection(newSel);
            if (!oldSel.empty() && oldSel == newSel)
                sim->centerSelection();
        }
        else if (button == RightButton)
        {
            float pickX, pickY;
            float aspectRatio = ((float) width / (float) height);
            (*activeView)->mapWindowToView((float) x / (float) width,
                                        (float) y / (float) height,
                                        pickX, pickY);
            Vector3f pickRay =
                sim->getActiveObserver()->getPickRay(pickX * aspectRatio, pickY);

            Selection sel = sim->pickObject(pickRay, renderer->getRenderFlags(), pickTolerance);
            if (!sel.empty())
            {
                if (contextMenuCallback != nullptr)
                    contextMenuCallback(x, y, sel);
            }
        }
        else if (button == MiddleButton)
        {
            if ((*activeView)->zoom != 1)
            {
                (*activeView)->alternateZoom = (*activeView)->zoom;
                (*activeView)->zoom = 1;
            }
            else
            {
                (*activeView)->zoom = (*activeView)->alternateZoom;
            }
            setFOVFromZoom();

            // If AutoMag, adapt the faintestMag to the new fov
            if((renderer->getRenderFlags() & Renderer::ShowAutoMag) != 0)
                setFaintestAutoMag();
        }
    }
}

void CelestiaCore::mouseWheel(float motion, int modifiers)
{
    setViewChanged();

    if (config->reverseMouseWheel) motion = -motion;
    if (motion != 0.0)
    {
        if ((modifiers & ShiftKey) != 0)
        {
            zoomTime = currentTime;
            zoomMotion = 0.25f * motion;
        }
        else
        {
            dollyTime = currentTime;
            dollyMotion = 0.25f * motion;
        }
    }
}

/// Handles cursor shape changes on view borders if the cursorHandler is defined.
/// This must be called on mouse move events on the OpenGL Widget.
/// x and y are the pixel coordinates relative to the widget.
void CelestiaCore::mouseMove(float x, float y)
{
#ifdef CELX
    if (luaHook && luaHook->callLuaHook(this, "mousemove", x, y))
        return;
#endif

    if (views.size() > 1 && cursorHandler != nullptr)
    {
        /*View* v1 = 0;     Unused*/
        /*View* v2 = 0;     Unused*/

        for (const auto v : views)
        {
            if (v->type == View::ViewWindow)
            {
                float vx, vy, vxp, vyp;
                vx = (x / width - v->x) / v->width;
                vy = ((1 - y / height) - v->y ) / v->height;
                vxp = vx * v->width * width;
                vyp = vy * v->height * height;

                if (vx >=0 && vx <= 1 && (abs(vyp) <= 2 || abs(vyp - v->height * height) <= 2))
                {
                    cursorHandler->setCursorShape(CelestiaCore::SizeVerCursor);
                    return;
                }
                if (vy >=0 && vy <= 1 && (abs(vxp) <= 2 || abs(vxp - v->width * width) <= 2))
                {
                    cursorHandler->setCursorShape(CelestiaCore::SizeHorCursor);
                    return;
                }
            }
        }
        cursorHandler->setCursorShape(defaultCursorShape);
    }
}

void CelestiaCore::mouseMove(float dx, float dy, int modifiers)
{
    if (modifiers != 0)
        setViewChanged();

    if (resizeSplit != nullptr)
    {
        switch(resizeSplit->type) {
        case View::HorizontalSplit:
            if (   resizeSplit->walkTreeResizeDelta(resizeSplit->child1, dy / height, true)
                && resizeSplit->walkTreeResizeDelta(resizeSplit->child2, dy / height, true))
            {
                resizeSplit->walkTreeResizeDelta(resizeSplit->child1, dy / height, false);
                resizeSplit->walkTreeResizeDelta(resizeSplit->child2, dy / height, false);
            }
            break;
        case View::VerticalSplit:
            if (   resizeSplit->walkTreeResizeDelta(resizeSplit->child1, dx / width, true)
                && resizeSplit->walkTreeResizeDelta(resizeSplit->child2, dx / width, true)
            )
            {
                resizeSplit->walkTreeResizeDelta(resizeSplit->child1, dx / width, false);
                resizeSplit->walkTreeResizeDelta(resizeSplit->child2, dx / width, false);
            }
            break;
        case View::ViewWindow:
            break;
        }
        setFOVFromZoom();
        return;
    }

#ifdef CELX
    if (luaHook &&
            luaHook->callLuaHook(this,"mousebuttonmove", dx, dy, modifiers))
    {
            return;
    }
#endif

    if ((modifiers & (LeftButton | RightButton)) != 0)
    {
        if (editMode && checkMask(modifiers, LeftButton | ShiftKey | ControlKey))
        {
            // Rotate the selected object
            Selection sel = sim->getSelection();
            Quaternionf q = Quaternionf::Identity();
            if (sel.getType() == Selection::Type_DeepSky)
                q = sel.deepsky()->getOrientation();
            else if (sel.getType() == Selection::Type_Body)
                q = sel.body()->getGeometryOrientation();

            q = XRotation(dy / height) * YRotation(dx / width) * q;

            if (sel.getType() == Selection::Type_DeepSky)
                sel.deepsky()->setOrientation(q);
            else if (sel.getType() == Selection::Type_Body)
                sel.body()->setGeometryOrientation(q);
        }
        else if (editMode && checkMask(modifiers, RightButton | ShiftKey | ControlKey))
        {
            // Rotate the selected object about an axis from its center to the
            // viewer.
            Selection sel = sim->getSelection();
            if (sel.deepsky() != nullptr)
            {
                double t = sim->getTime();
                Vector3d v = sel.getPosition(t).offsetFromKm(sim->getObserver().getPosition());
                Vector3f axis = v.cast<float>().normalized();

                Quaternionf r(AngleAxisf(dx / width, axis));

                Quaternionf q = sel.deepsky()->getOrientation();
                sel.deepsky()->setOrientation(r * q);
            }
        }
        else if (checkMask(modifiers, LeftButton | RightButton) ||
                 checkMask(modifiers, LeftButton | ControlKey))
        {
            // Y-axis controls distance (exponentially), and x-axis motion
            // rotates the camera about the view normal.
            float amount = dy / height;
            sim->changeOrbitDistance(amount * 5);
            if (dx * dx > dy * dy)
            {
                Observer& observer = sim->getObserver();
                Vector3d v(0, 0, dx * -MouseRotationSensitivity);
                v *= 0.5;

                Quaterniond obsOrientation = observer.getOrientation();
                Quaterniond dr = Quaterniond(0.0, v.x(), v.y(), v.z()) * obsOrientation;
                obsOrientation = Quaterniond(dr.coeffs() + obsOrientation.coeffs());
                obsOrientation.normalize();
                observer.setOrientation(obsOrientation);
            }
        }
        else if (checkMask(modifiers, LeftButton | ShiftKey))
        {
            // Mouse zoom control
            float amount = dy / height;
            float minFOV = MinimumFOV;
            float maxFOV = MaximumFOV;
            float fov = sim->getActiveObserver()->getFOV();

            // In order for the zoom to have the right feel, it should be
            // exponential.
            float newFOV = minFOV + (float) exp(log(fov - minFOV) + amount * 4);
            if (newFOV > maxFOV)
                newFOV = maxFOV;
            if (newFOV > minFOV)
            {
                sim->getActiveObserver()->setFOV(newFOV);
                setZoomFromFOV();
            }

            if ((renderer->getRenderFlags() & Renderer::ShowAutoMag))
            {
                setFaintestAutoMag();
                flash(fmt::sprintf(_("Magnitude limit: %.2f"), sim->getFaintestVisible()));
            }
        }
        else
        {
            // For a small field of view, rotate the camera more finely
            float coarseness = 1.5f;
            if ((modifiers & RightButton) == 0)
            {
                coarseness = radToDeg(sim->getActiveObserver()->getFOV()) / 30.0f;
            }
            else
            {
                // If right dragging to rotate, adjust the rotation rate
                // based on the distance from the reference object.
                coarseness = ComputeRotationCoarseness(*sim);
            }

            Quaternionf q = XRotation(dy / height * coarseness) * YRotation(dx / width * coarseness);
            if ((modifiers & RightButton) != 0)
                sim->orbit(q);
            else
                sim->rotate(q.conjugate());
        }

        mouseMotion += abs(dy) + abs(dx);
    }
}

/// Makes the view under x, y the active view.
void CelestiaCore::pickView(float x, float y)
{
    if (x+2 < (*activeView)->x * width || x-2 > ((*activeView)->x + (*activeView)->width) * width
        || (height - y)+2 < (*activeView)->y * height ||  (height - y)-2 > ((*activeView)->y + (*activeView)->height) * height)
    {
        activeView = views.begin();
        while ( (activeView != views.end())
                &&
                ( (x+2 < (*activeView)->x * width || x-2 > ((*activeView)->x + (*activeView)->width) * width || (height - y)+2 < (*activeView)->y * height ||  (height - y)-2 > ((*activeView)->y + (*activeView)->height) * height)
                  ||
                  ((*activeView)->type != View::ViewWindow)
                )
              )
        {
                activeView++;
        }

        // Make sure that we're left with a valid view
        if (activeView == views.end())
        {
            activeView = views.begin();
        }

        sim->setActiveObserver((*activeView)->observer);
        if (!showActiveViewFrame)
            flashFrameStart = currentTime;
        return;
    }
}

void CelestiaCore::joystickAxis(int axis, float amount)
{
    setViewChanged();

    float deadZone = 0.25f;

    if (abs(amount) < deadZone)
        amount = 0.0f;
    else
        amount = (amount - deadZone) * (1.0f / (1.0f - deadZone));

    amount = sign(amount) * square(amount);

    if (axis == Joy_XAxis)
        joystickRotation.y() = amount;
    else if (axis == Joy_YAxis)
        joystickRotation.x() = -amount;
}


void CelestiaCore::joystickButton(int button, bool down)
{
    setViewChanged();

    if (button >= 0 && button < JoyButtonCount)
        joyButtonsPressed[button] = down;
}


static void scrollConsole(Console& con, int lines)
{
    int topRow = con.getWindowRow();
    int height = con.getHeight();

    if (lines < 0)
    {
        if (topRow + lines > -height)
            console.setWindowRow(topRow + lines);
        else
            console.setWindowRow(-(height - 1));
    }
    else
    {
        if (topRow + lines <= -ConsolePageRows)
            console.setWindowRow(topRow + lines);
        else
            console.setWindowRow(-ConsolePageRows);
    }
}


void CelestiaCore::keyDown(int key, int modifiers)
{
    setViewChanged();

#ifdef CELX
    // TODO: should pass modifiers as a Lua table
    if (luaHook && luaHook->callLuaHook(this,
                                        "keydown",
                                        (float) key, (float) modifiers))
    {
        return;
    }
#endif
    switch (key)
    {
    case Key_F1:
        sim->setTargetSpeed(0);
        break;
    case Key_F2:
        sim->setTargetSpeed(1.0f);
        break;
    case Key_F3:
        sim->setTargetSpeed(1000.0f);
        break;
    case Key_F4:
        sim->setTargetSpeed((float) astro::speedOfLight);
        break;
    case Key_F5:
        sim->setTargetSpeed((float) astro::speedOfLight * 10.0f);
        break;
    case Key_F6:
        sim->setTargetSpeed(astro::AUtoKilometers(1.0f));
        break;
    case Key_F7:
        sim->setTargetSpeed(astro::lightYearsToKilometers(1.0f));
        break;
    case Key_F11:
        if (movieCapture != nullptr)
        {
            if (isRecording())
                recordPause();
            else
                recordBegin();
        }
        break;
    case Key_F12:
        if (movieCapture != nullptr)
            recordEnd();
        break;
    case Key_NumPad2:
    case Key_NumPad4:
    case Key_NumPad6:
    case Key_NumPad7:
    case Key_NumPad8:
    case Key_NumPad9:
        sim->setTargetSpeed(sim->getTargetSpeed());
        break;

    case Key_Down:
        if (showConsole)
            scrollConsole(console, 1);
        break;

    case Key_Up:
        if (showConsole)
            scrollConsole(console, -1);
        break;

    case Key_PageDown:
        if (showConsole)
            scrollConsole(console, ConsolePageRows);
        else
            back();
        break;

    case Key_PageUp:
        if (showConsole)
            scrollConsole(console, -ConsolePageRows);
        else
            forward();
        break;
    }

    if (KeyAccel < fMaxKeyAccel)
        KeyAccel *= 1.1;

    // Only process alphanumeric keys if we're not in text enter mode
    if (islower(key))
        key = toupper(key);
    if (!(key >= 'A' && key <= 'Z' && (textEnterMode != KbNormal) ))
    {
        if (modifiers & ShiftKey)
            shiftKeysPressed[key] = true;
        else
            keysPressed[key] = true;
    }
}

void CelestiaCore::keyUp(int key, int)
{
    setViewChanged();
    KeyAccel = 1.0;
    if (islower(key))
        key = toupper(key);
    keysPressed[key] = false;
    shiftKeysPressed[key] = false;
}

#ifdef CELX
static string getKeyName(const char* c, int modifiers)
{
    unsigned int length = strlen(c);

    // Translate control characters
    if (length == 1 && c[0] >= '\001' && c[0] <= '\032')
    {
        return fmt::sprintf("C-%c", '\140' + c[0]);
    }

    if (modifiers & CelestiaCore::ControlKey)
    {
        return fmt::sprintf("C-%s", c);
    }

    return string(c);
}
#endif

void CelestiaCore::charEntered(char c, int modifiers)
{
    setViewChanged();
    char C[2];
    C[0] = c;
    C[1] = '\0';
    charEntered(C, modifiers);
}

void CelestiaCore::charEntered(const char *c_p, int modifiers)
{
    setViewChanged();

    Observer* observer = sim->getActiveObserver();

    char c = *c_p;


#ifdef CELX
    if (celxScript != nullptr && (textEnterMode & KbPassToScript))
    {
        if (c != '\033' && celxScript->charEntered(c_p))
        {
            return;
        }
    }

#endif

    if (textEnterMode & KbAutoComplete)
    {
        wchar_t wc = 0; // Null wide character
        UTF8Decode(c_p, 0, strlen(c_p), wc);
#ifdef TARGET_OS_MAC
        if ( wc && (!iscntrl(wc)) )
#else
        if ( wc && (!iswcntrl(wc)) )
#endif
        {
            setTypedText(c_p);
        }
        else if (c == '\b')
        {
            typedTextCompletionIdx = -1;
            if (typedText.size() > 0)
            {
#ifdef AUTO_COMPLETION
                do
                {
#endif
                    // We remove bytes like b10xxx xxxx at the end of typeText
                    // these are guarantied to not be the first byte of a UTF-8 char
                    while (typedText.size() && ((typedText[typedText.size() - 1] & 0xC0) == 0x80)) {
                        typedText = string(typedText, 0, typedText.size() - 1);
                    }
                    // We then remove the first byte of the last UTF-8 char of typedText.
                    typedText = string(typedText, 0, typedText.size() - 1);
                    if (typedText.size() > 0)
                    {
                        typedTextCompletion = sim->getObjectCompletion(typedText, (renderer->getLabelMode() & Renderer::LocationLabels) != 0);
                    } else {
                        typedTextCompletion.clear();
                    }
#ifdef AUTO_COMPLETION
                } while (typedText.size() > 0 && typedTextCompletion.size() == 1);
#endif
            }
        }
        else if (c == '\011') // TAB
        {
            if (typedTextCompletionIdx + 1 < (int) typedTextCompletion.size())
                typedTextCompletionIdx++;
            else if ((int) typedTextCompletion.size() > 0 && typedTextCompletionIdx + 1 == (int) typedTextCompletion.size())
                typedTextCompletionIdx = 0;
            if (typedTextCompletionIdx >= 0) {
                string::size_type pos = typedText.rfind('/', typedText.length());
                if (pos != string::npos)
                    typedText = typedText.substr(0, pos + 1) + typedTextCompletion[typedTextCompletionIdx];
                else
                    typedText = typedTextCompletion[typedTextCompletionIdx];
            }
        }
        else if (c == Key_BackTab)
        {
            if (typedTextCompletionIdx > 0)
                typedTextCompletionIdx--;
            else if (typedTextCompletionIdx == 0)
                typedTextCompletionIdx = typedTextCompletion.size() - 1;
            else if (typedTextCompletion.size() > 0)
                typedTextCompletionIdx = typedTextCompletion.size() - 1;
            if (typedTextCompletionIdx >= 0) {
                string::size_type pos = typedText.rfind('/', typedText.length());
                if (pos != string::npos)
                    typedText = typedText.substr(0, pos + 1) + typedTextCompletion[typedTextCompletionIdx];
                else
                    typedText = typedTextCompletion[typedTextCompletionIdx];
            }
        }
        else if (c == '\033') // ESC
        {
            setTextEnterMode(textEnterMode & ~KbAutoComplete);
        }
        else if (c == '\n' || c == '\r')
        {
            if (typedText != "")
            {
                Selection sel = sim->findObjectFromPath(typedText, true);
                if (sel.empty() && typedTextCompletion.size() > 0)
                {
                    sel = sim->findObjectFromPath(typedTextCompletion[0], true);
                }
                if (!sel.empty())
                {
                    addToHistory();
                    sim->setSelection(sel);
                }
                typedText = "";
            }
            setTextEnterMode(textEnterMode & ~KbAutoComplete);
        }
        return;
    }

#ifdef CELX
    if (celxScript != nullptr)
    {
        if (c != '\033')
        {
            string keyName = getKeyName(c_p, modifiers);
            if (celxScript->handleKeyEvent(keyName.c_str()))
                return;
        }
    }

    if (luaHook)
    {
        string keyName = getKeyName(c_p, modifiers);
        if (luaHook->callLuaHook(this, "charentered", keyName.c_str()))
        {
            return;
        }
    }
#endif

    char C = toupper(c);
    switch (C)
    {
    case '\001': // Ctrl+A
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowAtmospheres);
        notifyWatchers(RenderFlagsChanged);
        break;

    case '\002': // Ctrl+B
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowBoundaries);
        notifyWatchers(RenderFlagsChanged);
        break;

    case '\n':
    case '\r':
        setTextEnterMode(textEnterMode | KbAutoComplete);
        break;

    case '\b':
        sim->setSelection(sim->getSelection().parent());
        break;

    case '\014': // Ctrl+L
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowNightMaps);
        notifyWatchers(RenderFlagsChanged);
        break;

    case '\013': // Ctrl+K
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowMarkers);
        if (renderer->getRenderFlags() & Renderer::ShowMarkers)
        {
            flash(_("Markers enabled"));
        }
        else
            flash(_("Markers disabled"));
        notifyWatchers(RenderFlagsChanged);
        break;

    case '\005':  // Ctrl+E
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowEclipseShadows);
        notifyWatchers(RenderFlagsChanged);
        break;

    case '\007':  // Ctrl+G
        flash(_("Goto surface"));
        addToHistory();
        sim->geosynchronousFollow();
        sim->gotoSurface(5.0);
        break;

    case '\006': // Ctrl+F
        addToHistory();
        altAzimuthMode = !altAzimuthMode;
        if (altAzimuthMode)
        {
            flash(_("Alt-azimuth mode enabled"));
        }
        else
            flash(_("Alt-azimuth mode disabled"));
        break;

    case 127: // Delete
        deleteView();
        break;

    case '\011': // TAB
        do
        {
            activeView++;
            if (activeView == views.end())
                activeView = views.begin();
        } while ((*activeView)->type != View::ViewWindow);
        sim->setActiveObserver((*activeView)->observer);
        if (!showActiveViewFrame)
            flashFrameStart = currentTime;
        break;

    case '\020':  // Ctrl+P
        if (!sim->getSelection().empty())
        {
            Selection sel = sim->getSelection();
            if (sim->getUniverse()->isMarked(sel, 1))
            {
                sim->getUniverse()->unmarkObject(sel, 1);
            }
            else
            {
                MarkerRepresentation markerRep(MarkerRepresentation::Diamond);
                markerRep.setSize(10.0f);
                markerRep.setColor(Color(0.0f, 1.0f, 0.0f, 0.9f));

                sim->getUniverse()->markObject(sel, markerRep, 1);
            }
        }
        break;

    case '\025': // Ctrl+U
        splitView(View::VerticalSplit);
        break;

    case '\022': // Ctrl+R
        splitView(View::HorizontalSplit);
        break;

    case '\004': // Ctrl+D
        singleView();
        break;

    case '\023':  // Ctrl+S
        renderer->setStarStyle((Renderer::StarStyle) (((int) renderer->getStarStyle() + 1) %
                                                      (int) Renderer::StarStyleCount));
        switch (renderer->getStarStyle())
        {
        case Renderer::FuzzyPointStars:
            flash(_("Star style: fuzzy points"));
            break;
        case Renderer::PointStars:
            flash(_("Star style: points"));
            break;
        case Renderer::ScaledDiscStars:
            flash(_("Star style: scaled discs"));
            break;
        default:
            break;
        }

        notifyWatchers(RenderFlagsChanged);
        break;

    case '\024':  // Ctrl+T
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowCometTails);
        if (renderer->getRenderFlags() & Renderer::ShowCometTails)
        {
            flash(_("Comet tails enabled"));
        }
        else
            flash(_("Comet tails disabled"));
        notifyWatchers(RenderFlagsChanged);
        break;

    case '\026':  // Ctrl+V
#ifdef USE_GLCONTEXT
        {
            GLContext* context = renderer->getGLContext();
            GLContext::GLRenderPath path = context->getRenderPath();
            GLContext::GLRenderPath newPath = context->nextRenderPath();

            if (newPath != path)
            {
                switch (newPath)
                {
                case GLContext::GLPath_GLSL:
                    flash(_("Render path: OpenGL 2.0"));
                    break;
                }
                context->setRenderPath(newPath);
                notifyWatchers(RenderFlagsChanged);
            }
        }
#endif
        break;

    case '\027':  // Ctrl+W
        wireframe = !wireframe;
        renderer->setRenderMode(wireframe ? GL_LINE : GL_FILL);
        break;

    case '\030':  // Ctrl+X
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowSmoothLines);
        if (renderer->getRenderFlags() & Renderer::ShowSmoothLines)
        {
            flash(_("Anti-aliasing enabled"));
            setFaintestAutoMag();
        }
        else
        {
            flash(_("Anti-aliasing disabled"));
        }
        notifyWatchers(RenderFlagsChanged);
        break;

    case '\031':  // Ctrl+Y
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowAutoMag);
        if (renderer->getRenderFlags() & Renderer::ShowAutoMag)
        {
            flash(_("Auto-magnitude enabled"));
            setFaintestAutoMag();
        }
        else
        {
            flash(_("Auto-magnitude disabled"));
        }
        notifyWatchers(RenderFlagsChanged);
        break;


    case '\033': // Escape
        cancelScript();
        addToHistory();
        if (textEnterMode != KbNormal)
        {
            setTextEnterMode(KbNormal);
        }
        else
        {
            if (sim->getObserverMode() == Observer::Travelling)
                sim->setObserverMode(Observer::Free);
            else
                sim->setFrame(ObserverFrame::Universal, Selection());
            if (!sim->getTrackedObject().empty())
                sim->setTrackedObject(Selection());
        }
        flash(_("Cancel"));
        break;

    case ' ':
        if (sim->getPauseState() == true)
        {
            if (scriptState == ScriptPaused)
                scriptState = ScriptRunning;
            sim->setPauseState(false);
        }
        else
        {
            sim->setPauseState(true);

            // If there's a script running then pause it.  This has the
            // potentially confusing side effect of rendering nonfunctional
            // goto, center, and other movement commands.
#ifdef CELX
            if (runningScript != nullptr || celxScript != nullptr)
#else
            if (runningScript != nullptr)
#endif
            {
                if (scriptState == ScriptRunning)
                    scriptState = ScriptPaused;
            }
            else
            {
                if (scriptState == ScriptPaused)
                    scriptState = ScriptRunning;
            }
        }

        if (sim->getPauseState() == true)
        {
            if (scriptState == ScriptPaused)
                flash(_("Time and script are paused"));
            else
                flash(_("Time is paused"));
        }
        else
        {
            flash(_("Resume"));
        }
        break;

    case '!':
        if (editMode)
        {
            showSelectionInfo(sim->getSelection());
        }
        else
        {
            time_t t = time(nullptr);
            struct tm *gmt = gmtime(&t);
            if (gmt != nullptr)
            {
                astro::Date d;
                d.year = gmt->tm_year + 1900;
                d.month = gmt->tm_mon + 1;
                d.day = gmt->tm_mday;
                d.hour = gmt->tm_hour;
                d.minute = gmt->tm_min;
                d.seconds = (int) gmt->tm_sec;
                sim->setTime(astro::UTCtoTDB(d));
            }
        }
        break;

    case '%':
        {
            const ColorTemperatureTable* current = renderer->getStarColorTable();

            if (current == GetStarColorTable(ColorTable_Enhanced))
            {
                renderer->setStarColorTable(GetStarColorTable(ColorTable_Blackbody_D65));
                flash(_("Star color: Blackbody D65"));
                notifyWatchers(RenderFlagsChanged);
            }
            else if (current == GetStarColorTable(ColorTable_Blackbody_D65))
            {
                renderer->setStarColorTable(GetStarColorTable(ColorTable_Enhanced));
                flash(_("Star color: Enhanced"));
                notifyWatchers(RenderFlagsChanged);
            }
            else
            {
                // Unknown color table
            }
        }
        break;

    case '^':
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowNebulae);
        notifyWatchers(RenderFlagsChanged);
        break;


    case '&':
        renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::LocationLabels);
        notifyWatchers(LabelFlagsChanged);
        break;

    case '*':
        addToHistory();
        sim->reverseObserverOrientation();
        break;

    case '?':
        addToHistory();
        if (!sim->getSelection().empty())
        {
            Vector3d v = sim->getSelection().getPosition(sim->getTime()).offsetFromKm(sim->getObserver().getPosition());
            int hours, mins;
            float secs;
            string buf;
            if (v.norm() >= 86400.0 * astro::speedOfLight)
            {
                // Light travel time in years, if >= 1day
                buf = fmt::sprintf(_("Light travel time:  %.4f yr"),
                                   astro::kilometersToLightYears(v.norm()));
            }
            else
            {
                // If Light travel delay < 1 day, display in [ hr : min : sec ]
                getLightTravelDelay(v.norm(), hours, mins, secs);
                if (hours == 0)
                {
                    buf = fmt::sprintf(_("Light travel time:  %d min  %.1f s"),
                                       mins, secs);
                }
                else
                {
                    buf = fmt::sprintf(_("Light travel time:  %d h  %d min  %.1f s"),
                                       hours, mins, secs);
                }
            }
            flash(buf, 2.0);
        }
        break;

    case '-':
        addToHistory();

        if (sim->getSelection().body() &&
            (sim->getTargetSpeed() < 0.99 * astro::speedOfLight))
        {
            Vector3d v = sim->getSelection().getPosition(sim->getTime()).offsetFromKm(sim->getObserver().getPosition());
            lightTravelFlag = !lightTravelFlag;
            if (lightTravelFlag)
            {
                flash(_("Light travel delay included"), 2.0);
                setLightTravelDelay(v.norm());
            }
            else
            {
                flash(_("Light travel delay switched off"), 2.0);
                setLightTravelDelay(-v.norm());
            }
        }
        else
        {
            flash(_("Light travel delay ignored"));
        }
        break;

    case ',':
        addToHistory();
        if (observer->getFOV() > MinimumFOV)
        {
            observer->setFOV(observer->getFOV() / 1.05f);
            setZoomFromFOV();
            if((renderer->getRenderFlags() & Renderer::ShowAutoMag))
            {
                setFaintestAutoMag();
                setlocale(LC_NUMERIC, "");
                string buf = fmt::sprintf(_("Magnitude limit: %.2f"), sim->getFaintestVisible());
                setlocale(LC_NUMERIC, "C");
                flash(buf);
            }
        }
        break;

    case '.':
        addToHistory();
        if (observer->getFOV() < MaximumFOV)
        {
            observer->setFOV(observer->getFOV() * 1.05f);
            setZoomFromFOV();
            if((renderer->getRenderFlags() & Renderer::ShowAutoMag) != 0)
            {
                setFaintestAutoMag();
                setlocale(LC_NUMERIC, "");
                string buf = fmt::sprintf(_("Magnitude limit: %.2f"), sim->getFaintestVisible());
                setlocale(LC_NUMERIC, "C");
                flash(buf);
            }
        }
        break;

    case '+':
        addToHistory();
        if (observer->getDisplayedSurface() != "")
        {
            observer->setDisplayedSurface("");
            flash(_("Using normal surface textures."));
        }
        else
        {
            observer->setDisplayedSurface("limit of knowledge");
            flash(_("Using limit of knowledge surface textures."));
        }
        break;

    case '/':
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowDiagrams);
        notifyWatchers(RenderFlagsChanged);
        break;

    case '0':
        addToHistory();
        sim->selectPlanet(-1);
        break;

    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        addToHistory();
        if (!(modifiers & ControlKey))
            sim->selectPlanet(c - '1');
        break;

    case ';':
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowCelestialSphere);
        notifyWatchers(RenderFlagsChanged);
        break;

    case '=':
        renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::ConstellationLabels);
        notifyWatchers(LabelFlagsChanged);
        break;

    case 'B':
        renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::StarLabels);
        notifyWatchers(LabelFlagsChanged);
        break;

    case 'C':
        addToHistory();
        if (c == 'c')
            sim->centerSelection();
        else
            sim->centerSelectionCO();
        break;

    case 'D':
        addToHistory();
        if (config->demoScriptFile != "")
           runScript(config->demoScriptFile);
        break;

    case 'E':
        if (c == 'e')
            renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::GalaxyLabels);
        else
            renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::GlobularLabels);
        notifyWatchers(LabelFlagsChanged);
        break;

    case 'F':
        addToHistory();
        flash(_("Follow"));
        sim->follow();
        break;

    case 'G':
        addToHistory();
        if (sim->getFrame()->getCoordinateSystem() == ObserverFrame::Universal)
            sim->follow();
        sim->gotoSelection(5.0, Vector3f::UnitY(), ObserverFrame::ObserverLocal);
        break;

    case 'H':
        addToHistory();
        sim->setSelection(sim->getUniverse()->getStarCatalog()->find(0));
        break;

    case 'I':
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowCloudMaps);
        notifyWatchers(RenderFlagsChanged);
        break;

    case 'J':
        addToHistory();
        sim->setTimeScale(-sim->getTimeScale());
        if (sim->getTimeScale() >= 0)
            flash(_("Time: Forward"));
        else
            flash(_("Time: Backward"));
        break;

    case 'K':
        addToHistory();
        if (abs(sim->getTimeScale()) > MinimumTimeRate)
        {
            if (c == 'k')
                sim->setTimeScale(sim->getTimeScale() / CoarseTimeScaleFactor);
            else
                sim->setTimeScale(sim->getTimeScale() / FineTimeScaleFactor);
            setlocale(LC_NUMERIC, "");
            string buf = fmt::sprintf(_("Time rate: %.6g"),  sim->getTimeScale()); // XXX %'.12g
            setlocale(LC_NUMERIC, "C");
            flash(buf);
        }
        break;

    case 'L':
        addToHistory();
        if (abs(sim->getTimeScale()) < MaximumTimeRate)
        {
            if (c == 'l')
                sim->setTimeScale(sim->getTimeScale() * CoarseTimeScaleFactor);
            else
                sim->setTimeScale(sim->getTimeScale() * FineTimeScaleFactor);
            setlocale(LC_NUMERIC, "");
            string buf = fmt::sprintf(_("Time rate: %.6g"),  sim->getTimeScale()); // XXX %'.12g
            setlocale(LC_NUMERIC, "C");
            flash(buf);
        }
        break;

    case 'M':
        if (c == 'm')
            renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::MoonLabels);
        else
            renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::MinorMoonLabels);
        notifyWatchers(LabelFlagsChanged);
        break;

    case 'N':
        renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::SpacecraftLabels);
        notifyWatchers(LabelFlagsChanged);
        break;

    case 'O':
        renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowOrbits);
        notifyWatchers(RenderFlagsChanged);
        break;

    case 'P':
        if (c == 'p')
            renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::PlanetLabels);
        else
            renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::DwarfPlanetLabels);
        notifyWatchers(LabelFlagsChanged);
        break;

    case 'R':
        if (c == 'r') // Skip rangechecking as setResolution does it already
            renderer->setResolution(renderer->getResolution() - 1);
        else
            renderer->setResolution(renderer->getResolution() + 1);
        switch (renderer->getResolution())
        {
        case 0:
            flash(_("Low res textures"));
            break;
        case 1:
            flash(_("Medium res textures"));
            break;
        case 2:
            flash(_("High res textures"));
            break;
        }
        notifyWatchers(RenderFlagsChanged); //how to synchronize with menu?
        break;

    case 'Q':
        sim->setTargetSpeed(-sim->getTargetSpeed());
        break;

    case 'S':
        sim->setTargetSpeed(0);
        break;

    case 'T':
        addToHistory();
        if (sim->getTrackedObject().empty())
            sim->setTrackedObject(sim->getSelection());
        else
            sim->setTrackedObject(Selection());
        break;

    case 'U':
        if (c == 'u')
            renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowGalaxies);
        else
            renderer->setRenderFlags(renderer->getRenderFlags() ^ Renderer::ShowGlobulars);
        notifyWatchers(RenderFlagsChanged);
        break;

    case 'V':
        setHudDetail((getHudDetail() + 1) % 3);
        break;

    case 'W':
        if (c == 'w')
            renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::AsteroidLabels);
        else
            renderer->setLabelMode(renderer->getLabelMode() ^ Renderer::CometLabels);
        notifyWatchers(LabelFlagsChanged);
        break;

    case 'X':
        sim->setTargetSpeed(sim->getTargetSpeed());
        break;

    case 'Y':
        flash(_("Sync Orbit"));
        addToHistory();
        sim->geosynchronousFollow();
        break;

    case ':':
        flash(_("Lock"));
        addToHistory();
        sim->phaseLock();
        break;

    case '"':
        flash(_("Chase"));
        addToHistory();
        sim->chase();
        break;

    case '[':
        if ((renderer->getRenderFlags() & Renderer::ShowAutoMag) == 0)
        {
            if (sim->getFaintestVisible() > 1.0f)
            {
                setFaintest(sim->getFaintestVisible() - 0.2f);
                notifyWatchers(FaintestChanged);
                setlocale(LC_NUMERIC, "");
                string buf = fmt::sprintf(_("Magnitude limit:  %.2f"),
                                          sim->getFaintestVisible());
                setlocale(LC_NUMERIC, "C");
                flash(buf);
            }
        }
        else if (renderer->getFaintestAM45deg() > 6.0f)
        {
            renderer->setFaintestAM45deg(renderer->getFaintestAM45deg() - 0.1f);
            setFaintestAutoMag();
            setlocale(LC_NUMERIC, "");
            string buf = fmt::sprintf(_("Auto magnitude limit at 45 degrees:  %.2f"),
                                      renderer->getFaintestAM45deg());
            setlocale(LC_NUMERIC, "C");
            flash(buf);
        }
        break;

    case '\\':
        addToHistory();
        sim->setTimeScale(1.0f);
        break;

    case ']':
        if((renderer->getRenderFlags() & Renderer::ShowAutoMag) == 0)
        {
            if (sim->getFaintestVisible() < 15.0f)
            {
                setFaintest(sim->getFaintestVisible() + 0.2f);
                notifyWatchers(FaintestChanged);
                setlocale(LC_NUMERIC, "");
                string buf = fmt::sprintf(_("Magnitude limit:  %.2f"),
                                          sim->getFaintestVisible());
                setlocale(LC_NUMERIC, "C");
                flash(buf);
            }
        }
        else if (renderer->getFaintestAM45deg() < 12.0f)
        {
            renderer->setFaintestAM45deg(renderer->getFaintestAM45deg() + 0.1f);
            setFaintestAutoMag();
            setlocale(LC_NUMERIC, "");
            string buf = fmt::sprintf(_("Auto magnitude limit at 45 degrees:  %.2f"),
                                      renderer->getFaintestAM45deg());
            setlocale(LC_NUMERIC, "C");
            flash(buf);
        }
        break;

    case '`':
        showFPSCounter = !showFPSCounter;
        break;

    case '{':
        {
            if (renderer->getAmbientLightLevel() > 0.05f)
                renderer->setAmbientLightLevel(renderer->getAmbientLightLevel() - 0.05f);
            else
                renderer->setAmbientLightLevel(0.0f);
            notifyWatchers(AmbientLightChanged);
            setlocale(LC_NUMERIC, "");
            string buf = fmt::sprintf(_("Ambient light level:  %.2f"),
                                      renderer->getAmbientLightLevel());
            setlocale(LC_NUMERIC, "C");
            flash(buf);
        }
        break;

    case '}':
        {
            if (renderer->getAmbientLightLevel() < 0.95f)
                renderer->setAmbientLightLevel(renderer->getAmbientLightLevel() + 0.05f);
            else
                renderer->setAmbientLightLevel(1.0f);
            notifyWatchers(AmbientLightChanged);
            setlocale(LC_NUMERIC, "");
            string buf = fmt::sprintf(_("Ambient light level:  %.2f"),
                                      renderer->getAmbientLightLevel());
            setlocale(LC_NUMERIC, "C");
            flash(buf);
        }
        break;

    case '(':
        {
            Galaxy::decreaseLightGain();
            setlocale(LC_NUMERIC, "");
            string buf = fmt::sprintf("%s:  %3.0f %%", _("Light gain"), Galaxy::getLightGain() * 100.0f);
            setlocale(LC_NUMERIC, "C");
            flash(buf);
            notifyWatchers(GalaxyLightGainChanged);
        }
        break;

    case ')':
        {
            Galaxy::increaseLightGain();
            setlocale(LC_NUMERIC, "");
            string buf = fmt::sprintf("%s:  %3.0f %%", _("Light gain"), Galaxy::getLightGain() * 100.0f);
            setlocale(LC_NUMERIC, "C");
            flash(buf);
            notifyWatchers(GalaxyLightGainChanged);
        }
        break;

    case '~':
        showConsole = !showConsole;
        break;

    case '@':
        // TODO: 'Edit mode' should be eliminated; it can be done better
        // with a Lua script.
        editMode = !editMode;
        break;

#ifdef USE_HDR
    case '|':
        renderer->setBloomEnabled(!renderer->getBloomEnabled());
        if (renderer->getBloomEnabled())
            flash(_("Bloom enabled"));
        else
            flash(_("Bloom disabled"));
        break;

    case '<':
        {
            renderer->decreaseBrightness();
            string buf = fmt::sprintf("%s:  %+3.2f", _("Exposure"), -renderer->getBrightness());
            flash(buf);
        }
        break;

    case '>':
        {
            renderer->increaseBrightness();
            string buf = fmt::sprintf("%s:  %+3.2f", _("Exposure"), -renderer->getBrightness());
            flash(buf);
        }
        break;
#endif
    }
}


void CelestiaCore::getLightTravelDelay(double distanceKm, int& hours, int& mins, float& secs)
{
    // light travel time in hours
    double lt = distanceKm / (3600.0 * astro::speedOfLight);
    hours = (int) lt;
    double mm = (lt - hours) * 60;
    mins = (int) mm;
    secs = (float) ((mm  - mins) * 60);
}


void CelestiaCore::setLightTravelDelay(double distanceKm)
{
    // light travel time in days
    double lt = distanceKm / (86400.0 * astro::speedOfLight);
    sim->setTime(sim->getTime() - lt);
}


bool CelestiaCore::getAltAzimuthMode() const
{
    return altAzimuthMode;
}

void CelestiaCore::setAltAzimuthMode(bool enable)
{
    altAzimuthMode = enable;
}

void CelestiaCore::start(double t)
{
    if (config->initScriptFile != "")
    {
        // using the KdeAlerter in runScript would create an infinite loop,
        // break it here by resetting config->initScriptFile:
        string filename = config->initScriptFile;
        config->initScriptFile = "";
        runScript(filename);
    }

    // Set the simulation starting time to the current system time
    sim->setTime(t);
    sim->update(0.0);

    sysTime = timer->getTime();

    if (startURL != "")
        goToUrl(startURL);
}

void CelestiaCore::setStartURL(string url)
{
    if (!url.substr(0,4).compare("cel:"))
    {
        startURL = url;
        config->initScriptFile = "";
    }
    else
    {
        config->initScriptFile = url;
    }
}


void CelestiaCore::tick()
{
    double lastTime = sysTime;
    sysTime = timer->getTime();

    // The time step is normally driven by the system clock; however, when
    // recording a movie, we fix the time step the frame rate of the movie.
    double dt = 0.0;
    if (movieCapture != nullptr && recording)
    {
        dt = 1.0 / movieCapture->getFrameRate();
    }
    else
    {
        dt = sysTime - lastTime;
    }

    // Pause script execution
    if (scriptState == ScriptPaused)
        dt = 0.0;

    currentTime += dt;

    // Mouse wheel zoom
    if (zoomMotion != 0.0f)
    {
        double span = 0.1;
#if 0
        double fraction;

        if (currentTime - zoomTime >= span)
            fraction = (zoomTime + span) - (currentTime - dt);
        else
            fraction = dt / span;
#endif

        // sim->changeOrbitDistance(zoomMotion * (float) fraction);
        if (currentTime - zoomTime >= span)
            zoomMotion = 0.0f;
    }

    // Mouse wheel dolly
    if (dollyMotion != 0.0)
    {
        double span = 0.1;
        double fraction;

        if (currentTime - dollyTime >= span)
            fraction = (dollyTime + span) - (currentTime - dt);
        else
            fraction = dt / span;

        sim->changeOrbitDistance((float) (dollyMotion * fraction));
        if (currentTime - dollyTime >= span)
            dollyMotion = 0.0f;
    }

    // Keyboard dolly
    if (keysPressed[Key_Home])
        sim->changeOrbitDistance((float) (-dt * 2));
    if (keysPressed[Key_End])
        sim->changeOrbitDistance((float) (dt * 2));

    // Keyboard rotate
    Vector3d av = sim->getObserver().getAngularVelocity();

    av = av * exp(-dt * RotationDecay);

    float fov = sim->getActiveObserver()->getFOV() / stdFOV;
    Selection refObject = sim->getFrame()->getRefObject();

    // Handle arrow keys; disable them when the log console is displayed,
    // because then they're used to scroll up and down.
    if (!showConsole)
    {
        if (!altAzimuthMode)
        {
            if (keysPressed[Key_Left])
                av += Vector3d::UnitZ() * (dt * -KeyRotationAccel);
            if (keysPressed[Key_Right])
                av += Vector3d::UnitZ() * (dt * KeyRotationAccel);
            if (keysPressed[Key_Down])
                av += Vector3d::UnitX() * (dt * fov * -KeyRotationAccel);
            if (keysPressed[Key_Up])
                av += Vector3d::UnitX() * (dt * fov * KeyRotationAccel);
        }
        else
        {
            if (!refObject.empty())
            {
                Quaterniond orientation = sim->getObserver().getOrientation();
                Vector3d up = sim->getObserver().getPosition().offsetFromKm(refObject.getPosition(sim->getTime()));
                up.normalize();

                Vector3d v = up * (KeyRotationAccel * dt);
                v = orientation * v;

                if (keysPressed[Key_Left])
                    av -= v;
                if (keysPressed[Key_Right])
                    av += v;
                if (keysPressed[Key_Down])
                    av += Vector3d::UnitX() * (dt * fov * -KeyRotationAccel);
                if (keysPressed[Key_Up])
                    av += Vector3d::UnitX() * (dt * fov * KeyRotationAccel);
            }
        }
    }

    if (keysPressed[Key_NumPad4])
        av += Vector3d(0.0, dt * fov * -KeyRotationAccel, 0.0);
    if (keysPressed[Key_NumPad6])
        av += Vector3d(0.0, dt * fov * KeyRotationAccel, 0.0);
    if (keysPressed[Key_NumPad2])
        av += Vector3d(dt * fov * -KeyRotationAccel, 0.0, 0.0);
    if (keysPressed[Key_NumPad8])
        av += Vector3d(dt * fov * KeyRotationAccel, 0.0, 0.0);
    if (keysPressed[Key_NumPad7] || joyButtonsPressed[JoyButton7])
        av += Vector3d(0.0, 0.0, dt * -KeyRotationAccel);
    if (keysPressed[Key_NumPad9] || joyButtonsPressed[JoyButton8])
        av += Vector3d(0.0, 0.0, dt * KeyRotationAccel);

    //Use Boolean to indicate if sim->setTargetSpeed() is called
    bool bSetTargetSpeed = false;
    if (joystickRotation != Vector3f::Zero())
    {
        bSetTargetSpeed = true;

        av += (dt * KeyRotationAccel) * joystickRotation.cast<double>();
        sim->setTargetSpeed(sim->getTargetSpeed());
    }

    if (keysPressed[Key_NumPad5])
        av = av * exp(-dt * RotationBraking);

    sim->getObserver().setAngularVelocity(av);

    if (keysPressed[(int)'A'] || joyButtonsPressed[JoyButton2])
    {
        bSetTargetSpeed = true;

        if (sim->getTargetSpeed() == 0.0f)
            sim->setTargetSpeed(0.1f);
        else
            sim->setTargetSpeed(sim->getTargetSpeed() * (float) exp(dt * 3));
    }
    if (keysPressed[(int)'Z'] || joyButtonsPressed[JoyButton1])
    {
        bSetTargetSpeed = true;

        sim->setTargetSpeed(sim->getTargetSpeed() / (float) exp(dt * 3));
    }
    if (!bSetTargetSpeed && av.norm() > 0.0f)
    {
        // Force observer velocity vector to align with observer direction if an observer
        // angular velocity still exists.
        sim->setTargetSpeed(sim->getTargetSpeed());
    }

    if (!refObject.empty())
    {
        Quaternionf q = Quaternionf::Identity();
        float coarseness = ComputeRotationCoarseness(*sim);

        if (shiftKeysPressed[Key_Left])
            q = q * YRotation((float) (dt * -KeyRotationAccel * coarseness));
        if (shiftKeysPressed[Key_Right])
            q = q * YRotation((float) (dt *  KeyRotationAccel * coarseness));
        if (shiftKeysPressed[Key_Up])
            q = q * XRotation((float) (dt * -KeyRotationAccel * coarseness));
        if (shiftKeysPressed[Key_Down])
            q = q * XRotation((float) (dt *  KeyRotationAccel * coarseness));
        sim->orbit(q);
    }

    // If there's a script running, tick it
    if (runningScript != nullptr)
    {
        bool finished = runningScript->tick(dt);
        if (finished)
            cancelScript();
    }

#ifdef CELX
    if (celxScript != nullptr)
    {
        celxScript->handleTickEvent(dt);
        if (scriptState == ScriptRunning)
        {
            bool finished = celxScript->tick(dt);
            if (finished)
                cancelScript();
        }
    }

    if (luaHook != nullptr)
        luaHook->callLuaHook(this, "tick", dt);
#endif // CELX

    sim->update(dt);
}


void CelestiaCore::draw()
{
    if (!viewUpdateRequired())
        return;
    viewChanged = false;

    if (views.size() == 1)
    {
        // I'm not certain that a special case for one view is required; but,
        // it's possible that there exists some broken hardware out there
        // that has to fall back to software rendering if the scissor test
        // is enable.  To keep performance on this hypothetical hardware
        // reasonable in the typical single view case, we'll use this
        // scissorless special case.  I'm only paranoid because I've been
        // burned by crap hardware so many times. cjl
        glViewport(0, 0, width, height);
        renderer->resize(width, height);
        sim->render(*renderer);
    }
    else
    {
        glEnable(GL_SCISSOR_TEST);
        for (const auto view : views)
        {
            if (view->type == View::ViewWindow)
            {
                glScissor((GLint) (view->x * width),
                          (GLint) (view->y * height),
                          (GLsizei) (view->width * width),
                          (GLsizei) (view->height * height));
                glViewport((GLint) (view->x * width),
                           (GLint) (view->y * height),
                           (GLsizei) (view->width * width),
                           (GLsizei) (view->height * height));
                renderer->resize((int) (view->width * width),
                                 (int) (view->height * height));
                sim->render(*renderer, *view->observer);
            }
        }
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, width, height);
    }

    GLboolean toggleAA = glIsEnabled(GL_MULTISAMPLE);
    if (toggleAA && (renderer->getRenderFlags() & Renderer::ShowCloudMaps))
        glDisable(GL_MULTISAMPLE);

    renderOverlay();
    if (showConsole)
    {
        console.setFont(font);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        console.begin();
        glTranslatef(0.0f, 200.0f, 0.0f);
        console.render(ConsolePageRows);
        console.end();
    }

    if (toggleAA)
        glEnable(GL_MULTISAMPLE);

    if (movieCapture != nullptr && recording)
        movieCapture->captureFrame();

    // Frame rate counter
    nFrames++;
    if (nFrames == 100 || sysTime - fpsCounterStartTime > 10.0)
    {
        fps = (double) nFrames / (sysTime - fpsCounterStartTime);
        nFrames = 0;
        fpsCounterStartTime = sysTime;
    }

#if 0
    GLenum err = glGetError();
    if (err != GL_NO_ERROR)
    {
        cout << _("GL error: ") << gluErrorString(err) << '\n';
    }
#endif
}


void CelestiaCore::resize(GLsizei w, GLsizei h)
{
    if (h == 0)
        h = 1;

    glViewport(0, 0, w, h);
    if (renderer != nullptr)
        renderer->resize(w, h);
    if (overlay != nullptr)
        overlay->setWindowSize(w, h);
    console.setScale(w, h);
    width = w;
    height = h;

    setFOVFromZoom();
#ifdef CELX
    if (luaHook && luaHook->callLuaHook(this,"resize", (float) w, (float) h))
        return;
#endif
}


// Return true if anything changed that requires re-rendering. Otherwise, we
// can skip rendering, keep the GPU idle, and save power.
bool CelestiaCore::viewUpdateRequired() const
{
#if 1
    // Enable after 1.5.0
    return true;
#else
    bool isPaused = sim->getPauseState() || sim->getTimeScale() == 0.0;

    // See if the camera in any of the views is moving
    bool observersMoving = false;
    for (const auto v : views)
    {
        if (v->observer->getAngularVelocity().length() > 1.0e-10 ||
            v->observer->getVelocity().length() > 1.0e-12)
        {
            observersMoving = true;
            break;
        }
    }

    if (viewChanged ||
        !isPaused ||
        observersMoving ||
        dollyMotion != 0.0 ||
        zoomMotion != 0.0 ||
        scriptState == ScriptRunning ||
        renderer->settingsHaveChanged())
    {
        return true;
    }
    else
    {
        return false;
    }
#endif
}


void CelestiaCore::setViewChanged()
{
    viewChanged = true;
}


void CelestiaCore::splitView(View::Type type, View* av, float splitPos)
{
    setViewChanged();

    if (av == nullptr)
        av = (*activeView);
    bool vertical = ( type == View::VerticalSplit );
    Observer* o = sim->addObserver();
    bool tooSmall = false;

    switch (type) // If active view is too small, don't split it.
    {
    case View::HorizontalSplit:
        if (av->height < 0.2f) tooSmall = true;
        break;
    case View::VerticalSplit:
        if (av->width < 0.2f) tooSmall = true;
        break;
    case View::ViewWindow:
        return;
        break;
    }

    if (tooSmall)
    {
        flash(_("View too small to be split"));
        return;
    }
    flash(_("Added view"));

    // Make the new observer a copy of the old one
    // TODO: This works, but an assignment operator for Observer
    // should be defined.
    *o = *(sim->getActiveObserver());

    float w1, h1, w2, h2;
    if (vertical)
    {
        w1 = av->width * splitPos;
        w2 = av->width - w1;
        h1 = av->height;
        h2 = av->height;
    }
    else
    {
        w1 = av->width;
        w2 = av->width;
        h1 = av->height * splitPos;
        h2 = av->height - h1;
    }

    View* split = new View(type,
                           0,
                           av->x,
                           av->y,
                           av->width,
                           av->height);
    split->parent = av->parent;
    if (av->parent != nullptr)
    {
        if (av->parent->child1 == av)
            av->parent->child1 = split;
        else
            av->parent->child2 = split;
    }
    split->child1 = av;

    av->width = w1;
    av->height = h1;
    av->parent = split;

    View* view = new View(View::ViewWindow,
                          o,
                          av->x + (vertical ? w1 : 0),
                          av->y + (vertical ? 0  : h1),
                          w2, h2);
    split->child2 = view;
    view->parent = split;
    view->zoom = av->zoom;

    views.push_back(split);
    views.push_back(view);

    setFOVFromZoom();
}

void CelestiaCore::setFOVFromZoom()
{
    for (const auto v : views)
        if (v->type == View::ViewWindow)
        {
            double fov = 2 * atan(height * v->height / (screenDpi / 25.4) / 2. / distanceToScreen) / v->zoom;
            v->observer->setFOV((float) fov);
        }
}

void CelestiaCore::setZoomFromFOV()
{
    for (auto v : views)
        if (v->type == View::ViewWindow)
        {
            v->zoom = (float) (2 * atan(height * v->height / (screenDpi / 25.4) / 2. / distanceToScreen) /  v->observer->getFOV());
        }
}

void CelestiaCore::singleView(View* av)
{
    setViewChanged();

    if (av == nullptr)
        av = (*activeView);

    list<View*>::iterator i = views.begin();
    while(i != views.end())
    {
        if ((*i) != av)
        {
            sim->removeObserver((*i)->observer);
            delete (*i)->observer;
            delete (*i);
            i=views.erase(i);
        }
        else
            i++;
    }

    av->x = 0.0f;
    av->y = 0.0f;
    av->width = 1.0f;
    av->height = 1.0f;
    av->parent = nullptr;
    av->child1 = nullptr;
    av->child2 = nullptr;

    activeView = views.begin();
    sim->setActiveObserver((*activeView)->observer);
    setFOVFromZoom();
}

void CelestiaCore::setActiveView(View* v)
{
    activeView = find(views.begin(),views.end(),v);
    sim->setActiveObserver((*activeView)->observer);
}

void CelestiaCore::deleteView(View* v)
{
    if (v == nullptr)
        v = (*activeView);

    if (v->parent == nullptr) return;

    //Erase view and parent view from views
    list<View*>::iterator i = views.begin();
    while(i != views.end())
    {
        if ((*i == v) || (*i == v->parent))
            i=views.erase(i);
        else
            i++;
    }

    int sign;
    View *sibling;
    if (v->parent->child1 == v)
    {
        sibling = v->parent->child2;
        sign = -1;
    }
    else
    {
        sibling = v->parent->child1;
        sign = 1;
    }
    sibling->parent = v->parent->parent;
    if (v->parent->parent != nullptr) {
        if (v->parent->parent->child1 == v->parent)
            v->parent->parent->child1 = sibling;
        else
            v->parent->parent->child2 = sibling;
    }

    v->walkTreeResize(sibling, sign);

    sim->removeObserver(v->observer);
    delete(v->observer);
    View* nextActiveView = sibling;
    while (nextActiveView->type != View::ViewWindow)
        nextActiveView = nextActiveView->child1;
    activeView = find(views.begin(),views.end(),nextActiveView);
    sim->setActiveObserver((*activeView)->observer);

    delete(v->parent);
    delete(v);

    if (!showActiveViewFrame)
        flashFrameStart = currentTime;
    setFOVFromZoom();
}

bool CelestiaCore::getFramesVisible() const
{
    return showViewFrames;
}

void CelestiaCore::setFramesVisible(bool visible)
{
    setViewChanged();

    showViewFrames = visible;
}

bool CelestiaCore::getActiveFrameVisible() const
{
    return showActiveViewFrame;
}

void CelestiaCore::setActiveFrameVisible(bool visible)
{
    setViewChanged();

    showActiveViewFrame = visible;
}


void CelestiaCore::setContextMenuCallback(ContextMenuFunc callback)
{
    contextMenuCallback = callback;
}


Renderer* CelestiaCore::getRenderer() const
{
    return renderer;
}

Simulation* CelestiaCore::getSimulation() const
{
    return sim;
}

void CelestiaCore::showText(string s,
                            int horig, int vorig,
                            int hoff, int voff,
                            double duration)
{
    messageText = s;
    messageHOrigin = horig;
    messageVOrigin = vorig;
    messageHOffset = hoff;
    messageVOffset = voff;
    messageStart = currentTime;
    messageDuration = duration;
}

int CelestiaCore::getTextWidth(string s) const
{
    return titleFont->getWidth(s);
}

static FormattedNumber SigDigitNum(double v, int digits)
{
    return FormattedNumber(v, digits,
                           FormattedNumber::GroupThousands |
                           FormattedNumber::SignificantDigits);
}


static string DistanceLyToStr(double distance)
{
    const char* units = "";

    if (abs(distance) >= astro::parsecsToLightYears(1e+6))
    {
        units = _("Mpc");
        distance = astro::lightYearsToParsecs(distance) / 1e+6;
    }
    else if (abs(distance) >= 0.5 * astro::parsecsToLightYears(1e+3))
    {
        units = _("kpc");
        distance = astro::lightYearsToParsecs(distance) / 1e+3;
    }
    else if (abs(distance) >= astro::AUtoLightYears(1000.0f))
    {
        units = _("ly");
    }
    else if (abs(distance) >= astro::kilometersToLightYears(10000000.0))
    {
        units = _("au");
        distance = astro::lightYearsToAU(distance);
    }
    else if (abs(distance) > astro::kilometersToLightYears(1.0f))
    {
        units = _("km");
        distance = astro::lightYearsToKilometers(distance);
    }
    else
    {
        units = _("m");
        distance = astro::lightYearsToKilometers(distance) * 1000.0f;
    }

    return fmt::sprintf("%s %s", SigDigitNum(distance, 5), units);
}


static string DistanceKmToStr(double distance)
{
    return DistanceLyToStr(astro::kilometersToLightYears(distance));
}


static void displayRotationPeriod(Overlay& overlay, double days)
{
    FormattedNumber n;
    const char *p;

    if (days > 1.0)
        n = FormattedNumber(days, 3, FormattedNumber::GroupThousands), p = _("days");
    else if (days > 1.0 / 24.0)
        n = FormattedNumber(days * 24.0, 3, FormattedNumber::GroupThousands), p = _("hours");
    else if (days > 1.0 / (24.0 * 60.0))
        n = FormattedNumber(days * 24.0 * 60.0, 3, FormattedNumber::GroupThousands), p = _("minutes");
    else
        n = FormattedNumber(days * 24.0 * 60.0 * 60.0, 3, FormattedNumber::GroupThousands), p = _("seconds");

    fmt::fprintf(overlay, _("Rotation period: %s %s\n"), n, p);
}

static void displaySpeed(Overlay& overlay, float speed)
{
    FormattedNumber n;
    const char *u;

    if (speed < 1.0f)
        n = SigDigitNum(speed * 1000.0f, 3), u = _("m/s");
    else if (speed < 10000.0f)
        n = SigDigitNum(speed, 3), u = _("km/s");
    else if (speed < (float) astro::speedOfLight * 100.0f)
        n = SigDigitNum(speed / astro::speedOfLight, 3), u = "c";
    else if (speed < astro::AUtoKilometers(1000.0f))
        n = SigDigitNum(astro::kilometersToAU(speed), 3), u = _("AU/s");
    else
        n = SigDigitNum(astro::kilometersToLightYears(speed), 3), u = _("ly/s");

    fmt::fprintf(overlay, _("Speed: %s %s\n"), n, u);
}

// Display a positive angle as degrees, minutes, and seconds. If the angle is less than one
// degree, only minutes and seconds are shown; if the angle is less than one minute, only
// seconds are displayed.
static string angleToStr(double angle)
{
    int degrees, minutes;
    double seconds;
    astro::decimalToDegMinSec(angle, degrees, minutes, seconds);

    if (degrees > 0)
    {
        return fmt::sprintf("%d%s %02d' %.1f\"",
                            degrees, UTF8_DEGREE_SIGN,
                            abs(minutes), abs(seconds));
    }

    if (minutes > 0)
    {
        return fmt::sprintf("%02d' %.1f\"",
                            abs(minutes), abs(seconds));
    }

    return fmt::sprintf("%.2f\"", abs(seconds));
}

static void displayDeclination(Overlay& overlay, double angle)
{
    int degrees, minutes;
    double seconds;
    astro::decimalToDegMinSec(angle, degrees, minutes, seconds);

    const char sign = angle < 0.0 ? '-' : '+';

    fmt::fprintf(overlay, "Dec: %c%d%s %02d' %.1f\"\n",
                          sign, abs(degrees), UTF8_DEGREE_SIGN,
                          abs(minutes), abs(seconds));
}


static void displayRightAscension(Overlay& overlay, double angle)
{
    int hours, minutes;
    double seconds;
    astro::decimalToHourMinSec(angle, hours, minutes, seconds);

    fmt::fprintf(overlay, "RA: %dh %02dm %.1fs\n",
                          hours, abs(minutes), abs(seconds));
}

static void displayApparentDiameter(Overlay& overlay,
                                    double radius,
                                    double distance)
{
    if (distance > radius)
    {
        double arcSize = radToDeg(asin(radius / distance) * 2.0);

        // Only display the arc size if it's less than 160 degrees and greater
        // than one second--otherwise, it's probably not interesting data.
        if (arcSize < 160.0 && arcSize > 1.0 / 3600.0)
        {
            fmt::fprintf(overlay, _("Apparent diameter: %s\n"),
                                 angleToStr(arcSize));
        }
    }
}

static void displayApparentMagnitude(Overlay& overlay,
                                     float absMag,
                                     double distance)
{
    if (distance > 32.6167)
    {
        float appMag = astro::absToAppMag(absMag, (float) distance);
        fmt::fprintf(overlay, _("Apparent magnitude: %.1f\n"), appMag);
    }
    else
    {
        fmt::fprintf(overlay, _("Absolute magnitude: %.1f\n"), absMag);
    }
}


static void displayRADec(Overlay& overlay, const Vector3d& v)
{
    double phi = atan2(v.x(), v.z()) - PI / 2;
    if (phi < 0)
        phi = phi + 2 * PI;

    double theta = atan2(sqrt(v.x() * v.x() + v.z() * v.z()), v.y());
    if (theta > 0)
        theta = PI / 2 - theta;
    else
        theta = -PI / 2 - theta;


    displayRightAscension(overlay, radToDeg(phi));
    displayDeclination(overlay, radToDeg(theta));
}


// Display nicely formatted planetocentric/planetographic coordinates.
// The latitude and longitude parameters are angles in radians, altitude
// is in kilometers.
static void displayPlanetocentricCoords(Overlay& overlay,
                                        const Body& body,
                                        double longitude,
                                        double latitude,
                                        double altitude,
                                        bool showAltitude)
{
    char ewHemi = ' ';
    char nsHemi = ' ';
    double lon = 0.0;
    double lat = 0.0;

    // Terrible hack for Earth and Moon longitude conventions.  Fix by
    // adding a field to specify the longitude convention in .ssc files.
    if (body.getName() == "Earth" || body.getName() == "Moon")
    {
        if (latitude < 0.0)
            nsHemi = 'S';
        else if (latitude > 0.0)
            nsHemi = 'N';

        if (longitude < 0.0)
            ewHemi = 'W';
        else if (longitude > 0.0f)
            ewHemi = 'E';

        lon = (float) abs(radToDeg(longitude));
        lat = (float) abs(radToDeg(latitude));
    }
    else
    {
        // Swap hemispheres if the object is a retrograde rotator
        Quaterniond q = body.getEclipticToEquatorial(astro::J2000);
        bool retrograde = (q * Vector3d::UnitY()).y() < 0.0;

        if ((latitude < 0.0) ^ retrograde)
            nsHemi = 'S';
        else if ((latitude > 0.0) ^ retrograde)
            nsHemi = 'N';

        if (retrograde)
            ewHemi = 'E';
        else
            ewHemi = 'W';

        lon = -radToDeg(longitude);
        if (lon < 0.0)
            lon += 360.0;
        lat = abs(radToDeg(latitude));
    }

    if (showAltitude)
        fmt::fprintf(overlay, "%.6f%c %.6f%c", lat, nsHemi, lon, ewHemi);
    else
        fmt::fprintf(overlay, _("%.6f%c %.6f%c %f km"), lat, nsHemi, lon, ewHemi, altitude);
}


#if 0
// Show the planetocentric latitude, longitude, and altitude of a
// observer.
static void displayObserverPlanetocentricCoords(Overlay& overlay,
                                                Body& body,
                                                const UniversalCoord& observerPos,
                                                double tdb)
{
    Vector3d ecl = observerPos.offsetFromKm(Selection(&body).getPosition(tdb));
    Vector3d pc = body.eclipticToPlanetocentric(ecl, tdb);

    displayPlanetocentricCoords(overlay, body, pc.x(), pc.y(), pc.z(), true);
}
#endif


static void displayStarInfo(Overlay& overlay,
                            int detail,
                            Star& star,
                            const Universe& universe,
                            double distance)
{
    fmt::fprintf(overlay, _("Distance: %s\n"), DistanceLyToStr(distance));

    if (!star.getVisibility())
    {
        overlay << _("Star system barycenter\n");
    }
    else
    {
        fmt::fprintf(overlay, _("Abs (app) mag: %.2f (%.2f)\n"),
                                star.getAbsoluteMagnitude(),
                                astro::absToAppMag(star.getAbsoluteMagnitude(),
                                                   float(distance)));

        if (star.getLuminosity() > 1.0e-10f)
            fmt::fprintf(overlay, _("Luminosity: %sx Sun\n"), SigDigitNum(star.getLuminosity(), 3));

        const char* star_class;
        switch (star.getSpectralType()[0])
        {
        case 'Q':
            star_class = _("Neutron star");
            break;
        case 'X':
            star_class = _("Black hole");
            break;
        default:
            star_class = star.getSpectralType();
        };
        fmt::fprintf(overlay, _("Class: %s\n"), star_class);

        displayApparentDiameter(overlay, star.getRadius(),
                                astro::lightYearsToKilometers(distance));

        if (detail > 1)
        {
            fmt::fprintf(overlay, _("Surface temp: %s K\n"), SigDigitNum(star.getTemperature(), 3));
            float solarRadii = star.getRadius() / 6.96e5f;

            if (solarRadii > 0.01f)
            {
                fmt::fprintf(overlay, _("Radius: %s Rsun  (%s km)\n"),
                             SigDigitNum(star.getRadius() / 696000.0f, 2),
                             SigDigitNum(star.getRadius(), 3));
            }
            else
            {
                fmt::fprintf(overlay, _("Radius: %s km\n"),
                             SigDigitNum(star.getRadius(), 3));
            }

            if (star.getRotationModel()->isPeriodic())
            {
                float period = (float) star.getRotationModel()->getPeriod();
                displayRotationPeriod(overlay, period);
            }
        }
    }

    if (detail > 1)
    {
        SolarSystem* sys = universe.getSolarSystem(&star);
        if (sys != nullptr && sys->getPlanets()->getSystemSize() != 0)
            overlay << _("Planetary companions present\n");
    }
}


static void displayDSOinfo(Overlay& overlay, const DeepSkyObject& dso, double distance)
{
    overlay << dso.getDescription() << '\n';

    if (distance >= 0)
    {
        fmt::fprintf(overlay, _("Distance: %s\n"),
                     DistanceLyToStr(distance));
    }
    else
    {
        fmt::fprintf(overlay, _("Distance from center: %s\n"),
                     DistanceLyToStr(distance + dso.getRadius()));
     }
    fmt::fprintf(overlay, _("Radius: %s\n"),
                 DistanceLyToStr(dso.getRadius()));

    displayApparentDiameter(overlay, dso.getRadius(), distance);
    if (dso.getAbsoluteMagnitude() > DSO_DEFAULT_ABS_MAGNITUDE)
    {
        displayApparentMagnitude(overlay,
                                 dso.getAbsoluteMagnitude(),
                                 distance);
    }
}


static void displayPlanetInfo(Overlay& overlay,
                              int detail,
                              Body& body,
                              double t,
                              double distanceKm,
                              const Vector3d& viewVec)
{
    double distance = distanceKm - body.getRadius();
    fmt::fprintf(overlay, _("Distance: %s\n"), DistanceKmToStr(distance));

    if (body.getClassification() == Body::Invisible)
    {
        return;
    }

    fmt::fprintf(overlay, _("Radius: %s\n"), DistanceKmToStr(body.getRadius()));

    displayApparentDiameter(overlay, body.getRadius(), distanceKm);

    // Display the phase angle

    // Find the parent star of the body. This can be slightly complicated if
    // the body orbits a barycenter instead of a star.
    Selection parent = Selection(&body).parent();
    while (parent.body() != nullptr)
        parent = parent.parent();

    if (parent.star() != nullptr)
    {
        bool showPhaseAngle = false;

        Star* sun = parent.star();
        if (sun->getVisibility())
        {
            showPhaseAngle = true;
        }
        else if (sun->getOrbitingStars())
        {
            // The planet's orbit is defined with respect to a barycenter. If there's
            // a single star orbiting the barycenter, we'll compute the phase angle
            // for the planet with respect to that star. If there are no stars, the
            // planet is an orphan, drifting through space with no star. We also skip
            // displaying the phase angle when there are multiple stars (for now.)
            if (sun->getOrbitingStars()->size() == 1)
            {
                sun = sun->getOrbitingStars()->at(0);
                showPhaseAngle = sun->getVisibility();
            }
        }

        if (showPhaseAngle)
        {
            Vector3d sunVec = Selection(&body).getPosition(t).offsetFromKm(Selection(sun).getPosition(t));
            sunVec.normalize();
            double cosPhaseAngle = sunVec.dot(viewVec.normalized());
            double phaseAngle = acos(cosPhaseAngle);
            fmt::fprintf(overlay, _("Phase angle: %.1f%s\n"), radToDeg(phaseAngle), UTF8_DEGREE_SIGN);
        }
    }

    if (detail > 1)
    {
        if (body.getRotationModel(t)->isPeriodic())
            displayRotationPeriod(overlay, body.getRotationModel(t)->getPeriod());

        if (body.getName() != "Earth")
        {
            if (body.getMass() > 0)
                fmt::fprintf(overlay, _("Mass: %.2f Me\n"), body.getMass());
        }

        float density = body.getDensity();
        if (density > 0)
        {
            fmt::fprintf(overlay, _("Density: %.2f x 1000 kg/m^3\n"), density / 1000.0);
        }


        float planetTemp = body.getTemperature(t);
        if (planetTemp > 0)
            fmt::fprintf(overlay, _("Temperature: %.0f K\n"), planetTemp);
    }
}


static void displayLocationInfo(Overlay& overlay,
                                Location& location,
                                double distanceKm)
{
    fmt::fprintf(overlay, _("Distance: %s\n"), DistanceKmToStr(distanceKm));

    Body* body = location.getParentBody();
    if (body != nullptr)
    {
        Vector3f locPos = location.getPosition();
        Vector3d lonLatAlt = body->cartesianToPlanetocentric(locPos.cast<double>());
        displayPlanetocentricCoords(overlay, *body,
                                    lonLatAlt.x(), lonLatAlt.y(), lonLatAlt.z(), false);
    }
}

static string getSelectionName(const Selection& sel, const Universe& univ)
{
    switch (sel.getType())
    {
    case Selection::Type_Body:
        return sel.body()->getName(false);
    case Selection::Type_DeepSky:
        return univ.getDSOCatalog()->getDSOName(sel.deepsky(), false);
    case Selection::Type_Star:
        return univ.getStarCatalog()->getStarName(*sel.star(), true);
    case Selection::Type_Location:
        return sel.location()->getName(false);
    default:
        return "";
    }
}

#if 0
static void displaySelectionName(Overlay& overlay,
                                 const Selection& sel,
                                 const Universe& univ)
{
    switch (sel.getType())
    {
    case Selection::Type_Body:
        overlay << sel.body()->getName(true);
        break;
    case Selection::Type_DeepSky:
        overlay << univ.getDSOCatalog()->getDSOName(sel.deepsky(), true);
        break;
    case Selection::Type_Star:
        //displayStarName(overlay, *(sel.star()), *univ.getStarCatalog());
        overlay << univ.getStarCatalog()->getStarName(*sel.star(), true);
        break;
    case Selection::Type_Location:
        overlay << sel.location()->getName(true);
        break;
    default:
        break;
    }
}
#endif


static void showViewFrame(const View* v, int width, int height)
{
    glBegin(GL_LINE_LOOP);
    glVertex3f(v->x * width, v->y * height, 0.0f);
    glVertex3f(v->x * width, (v->y + v->height) * height - 1, 0.0f);
    glVertex3f((v->x + v->width) * width - 1, (v->y + v->height) * height - 1, 0.0f);
    glVertex3f((v->x + v->width) * width - 1, v->y * height, 0.0f);
    glEnd();
}


void CelestiaCore::setScriptImage(float duration,
                                  float xoffset,
                                  float yoffset,
                                  float alpha,
                                  const string& filename,
                                  bool fitscreen)
{
    if (!image || !image->isNewImage(filename))
    {
        delete image;
        image = new CelestiaCore::OverlayImage(filename);
    }
    image->setStartTime((float) currentTime);
    image->setDuration(duration);
    image->setOffset(xoffset, yoffset);
    image->setAlpha(alpha);
    image->fitScreen(fitscreen);
}


CelestiaCore::OverlayImage::OverlayImage(string f)
{
    filename = std::move(f);
    delete texture;
    texture = LoadTextureFromFile(string("images/") + filename);
}


void CelestiaCore::OverlayImage::render(float curr_time, int width, int height)
{
    if (!texture || (curr_time >= start + duration))
        return;

    float xSize = texture->getWidth();
    float ySize = texture->getHeight();

    // center overlay image horizontally if offsetX = 0
    float left = (width * (1 + offsetX) - xSize)/2;
    // center overlay image vertically if offsetY = 0
    float bottom = (height * (1 + offsetY) - ySize)/2;

    if (fitscreen)
    {
        float coeffx = xSize / width;  // overlay pict width/view window width ratio
        float coeffy = ySize / height; // overlay pict height/view window height ratio
        xSize = xSize / coeffx;        // new overlay picture width size to fit viewport
        ySize = ySize / coeffy;        // new overlay picture height to fit viewport

        left = (width - xSize) / 2;    // to be sure overlay pict is centered in viewport
        bottom = 0;                    // overlay pict locked at bottom of screen
    }

    glEnable(GL_TEXTURE_2D);
    texture->bind();

    glBegin(GL_TRIANGLE_FAN);
    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(left, bottom);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(left + xSize, bottom);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(left + xSize, bottom + ySize);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(left, bottom + ySize);
    glEnd();
}


void CelestiaCore::renderOverlay()
{
#ifdef CELX
    if (luaHook) luaHook->callLuaHook(this, "renderoverlay");
#endif
    if (font == nullptr)
        return;

    overlay->setFont(font);

    int fontHeight = font->getHeight();
    int emWidth = font->getWidth("M");
    assert(emWidth > 0);

    overlay->begin();

#ifdef CELX
    if (runningScript || celxScript)
    {
#else
    if (runningScript)
    {
#endif
        if (image)
            image->render((float) currentTime, width, height);
    }

    if (views.size() > 1)
    {
        // Render a thin border arround all views
        if (showViewFrames || resizeSplit)
        {
            glLineWidth(1.0f);
            glDisable(GL_TEXTURE_2D);
            glColor4f(0.5f, 0.5f, 0.5f, 1.0f);
            for(const auto v : views)
                if (v->type == View::ViewWindow)
                    showViewFrame(v, width, height);
        }
        glLineWidth(1.0f);

        // Render a very simple border around the active view
        View* av = (*activeView);

        if (showActiveViewFrame)
        {
            glLineWidth(2.0f);
            glDisable(GL_TEXTURE_2D);
            glColor4f(0.5f, 0.5f, 1.0f, 1.0f);
            showViewFrame(av, width, height);
            glLineWidth(1.0f);
        }

        if (currentTime < flashFrameStart + 0.5)
        {
            glLineWidth(8.0f);
            glColor4f(0.5f, 0.5f, 1.0f,
                      (float) (1.0 - (currentTime - flashFrameStart) / 0.5));
            showViewFrame(av, width, height);
            glLineWidth(1.0f);
        }
    }

    setlocale(LC_NUMERIC, "");

    if (hudDetail > 0 && (overlayElements & ShowTime))
    {
        double lt = 0.0;

        if (sim->getSelection().getType() == Selection::Type_Body &&
            (sim->getTargetSpeed() < 0.99 * astro::speedOfLight))
        {
            if (lightTravelFlag)
            {
                Vector3d v = sim->getSelection().getPosition(sim->getTime()).offsetFromKm(sim->getObserver().getPosition());
                // light travel time in days
                lt = v.norm() / (86400.0 * astro::speedOfLight);
            }
        }

        double tdb = sim->getTime() + lt;
        astro::Date d = timeZoneBias != 0 ? astro::TDBtoLocal(tdb) : astro::TDBtoUTC(tdb);
        const char* dateStr = d.toCStr(dateFormat);
        int dateWidth = (font->getWidth(dateStr)/(emWidth * 3) + 2) * emWidth * 3;
        if (dateWidth > dateStrWidth) dateStrWidth = dateWidth;

        // Time and date
        glPushMatrix();
        glColor4f(0.7f, 0.7f, 1.0f, 1.0f);
        glTranslatef( (float) (width - dateStrWidth),
                      (float) (height - fontHeight),
                      0.0f);
        overlay->beginText();

        overlay->print(dateStr);

        if (lightTravelFlag && lt > 0.0)
        {
            glColor4f(0.42f, 1.0f, 1.0f, 1.0f);
            *overlay << _("  LT");
            glColor4f(0.7f, 0.7f, 1.0f, 1.0f);
        }
        *overlay << '\n';

        {
            if (abs(abs(sim->getTimeScale()) - 1) < 1e-6)
            {
                if (sign(sim->getTimeScale()) == 1)
                    *overlay << _("Real time");
                else
                    *overlay << _("-Real time");
            }
            else if (abs(sim->getTimeScale()) < MinimumTimeRate)
            {
                *overlay << _("Time stopped");
            }
            else if (abs(sim->getTimeScale()) > 1.0)
            {
                fmt::fprintf(*overlay, _("%.6g x faster"), sim->getTimeScale()); // XXX: %'.12g
            }
            else
            {
                fmt::fprintf(*overlay, _("%.6g x slower"), 1.0 / sim->getTimeScale()); // XXX: %'.12g
            }

            if (sim->getPauseState() == true)
            {
                glColor4f(1.0f, 0.0f, 0.0f, 1.0f);
                *overlay << _(" (Paused)");
            }
        }

        overlay->endText();
        glPopMatrix();
    }

    if (hudDetail > 0 && (overlayElements & ShowVelocity))
    {
        // Speed
        glPushMatrix();
        glTranslatef(0.0f, (float) (fontHeight * 2 + 5), 0.0f);
        glColor4f(0.7f, 0.7f, 1.0f, 1.0f);

        overlay->beginText();
        *overlay << '\n';
        if (showFPSCounter)
#ifdef OCTREE_DEBUG
            fmt::fprintf(*overlay, _("FPS: %.1f, vis. stars stats: [ %zu : %zu : %zu ], vis. DSOs stats: [ %zu : %zu : %zu ]\n"),
                         fps,
                         getRenderer()->m_starProcStats.objects,
                         getRenderer()->m_starProcStats.nodes,
                         getRenderer()->m_starProcStats.height,
                         getRenderer()->m_dsoProcStats.objects,
                         getRenderer()->m_dsoProcStats.nodes,
                         getRenderer()->m_dsoProcStats.height);
#else
            fmt::fprintf(*overlay, _("FPS: %.1f\n"), fps);
#endif
        else
            *overlay << '\n';

        displaySpeed(*overlay, sim->getObserver().getVelocity().norm());

        overlay->endText();
        glPopMatrix();
    }

    Universe *u = sim->getUniverse();

    if (hudDetail > 0 && (overlayElements & ShowFrame))
    {
        // Field of view and camera mode in lower right corner
        glPushMatrix();
        glTranslatef((float) (width - emWidth * 15),
                     (float) (fontHeight * 3 + 5), 0.0f);
        overlay->beginText();
        glColor4f(0.6f, 0.6f, 1.0f, 1);

        if (sim->getObserverMode() == Observer::Travelling)
        {
            double timeLeft = sim->getArrivalTime() - sim->getRealTime();
            if (timeLeft >= 1)
                fmt::fprintf(*overlay, _("Travelling (%s)\n"),
                             FormattedNumber(timeLeft, 0, FormattedNumber::GroupThousands));
            else
                fmt::fprintf(*overlay, _("Travelling\n"));
        }
        else
        {
            *overlay << '\n';
        }

        if (!sim->getTrackedObject().empty())
        {
            fmt::fprintf(*overlay, _("Track %s\n"),
                         C_("Track", getSelectionName(sim->getTrackedObject(), *u)));
        }
        else
        {
            *overlay << '\n';
        }

        {
            //FrameOfReference frame = sim->getFrame();
            Selection refObject = sim->getFrame()->getRefObject();
            ObserverFrame::CoordinateSystem coordSys = sim->getFrame()->getCoordinateSystem();

            switch (coordSys)
            {
            case ObserverFrame::Ecliptical:
                fmt::fprintf(*overlay, _("Follow %s\n"),
                             C_("Follow", getSelectionName(refObject, *u)));
                break;
            case ObserverFrame::BodyFixed:
                fmt::fprintf(*overlay, _("Sync Orbit %s\n"),
                             C_("Sync", getSelectionName(refObject, *u)));
                break;
            case ObserverFrame::PhaseLock:
                fmt::fprintf(*overlay, _("Lock %s -> %s\n"),
                             C_("Lock", getSelectionName(refObject, *u)),
                             C_("LockTo", getSelectionName(sim->getFrame()->getTargetObject(), *u)));
                break;

            case ObserverFrame::Chase:
                fmt::fprintf(*overlay, _("Chase %s\n"),
                             C_("Chase", getSelectionName(refObject, *u)));
                break;

            default:
                *overlay << '\n';
                break;
            }
        }

        glColor4f(0.7f, 0.7f, 1.0f, 1.0f);

        // Field of view
        float fov = radToDeg(sim->getActiveObserver()->getFOV());
        fmt::fprintf(*overlay, _("FOV: %s (%.2fx)\n"),
                              angleToStr(fov), (*activeView)->zoom);
        overlay->endText();
        glPopMatrix();
    }

    // Selection info
    Selection sel = sim->getSelection();
    if (!sel.empty() && hudDetail > 0 && (overlayElements & ShowSelection))
    {
        glPushMatrix();
        glColor4f(0.7f, 0.7f, 1.0f, 1.0f);
        glTranslatef(0.0f, (float) (height - titleFont->getHeight()), 0.0f);

        overlay->beginText();
        Vector3d v = sel.getPosition(sim->getTime()).offsetFromKm(sim->getObserver().getPosition());

        switch (sel.getType())
        {
        case Selection::Type_Star:
            {
                if (sel != lastSelection)
                {
                    lastSelection = sel;
                    selectionNames = sim->getUniverse()->getStarCatalog()->getStarNameList(*sel.star());
                    // Skip displaying the English name if a localized version is present.
                    string starName = sim->getUniverse()->getStarCatalog()->getStarName(*sel.star());
                    string locStarName = sim->getUniverse()->getStarCatalog()->getStarName(*sel.star(), true);
                    if (sel.star()->getCatalogNumber() == 0 && selectionNames.find("Sun") != string::npos && (const char*) "Sun" != _("Sun"))
                    {
                        string::size_type startPos = selectionNames.find("Sun");
                        string::size_type endPos = selectionNames.find(_("Sun"));
                        selectionNames = selectionNames.erase(startPos, endPos - startPos);
                    }
                    else if (selectionNames.find(starName) != string::npos && starName != locStarName)
                    {
                        string::size_type startPos = selectionNames.find(locStarName);
                        selectionNames = selectionNames.substr(startPos);
                    }
                }

                overlay->setFont(titleFont);
                *overlay << selectionNames;
                overlay->setFont(font);
                *overlay << '\n';
                displayStarInfo(*overlay,
                                hudDetail,
                                *(sel.star()),
                                *(sim->getUniverse()),
                                astro::kilometersToLightYears(v.norm()));
            }
            break;

        case Selection::Type_DeepSky:
            {
                if (sel != lastSelection)
                {
                    lastSelection = sel;
                    selectionNames = sim->getUniverse()->getDSOCatalog()->getDSONameList(sel.deepsky());
                    // Skip displaying the English name if a localized version is present.
                    string DSOName = sim->getUniverse()->getDSOCatalog()->getDSOName(sel.deepsky());
                    string locDSOName = sim->getUniverse()->getDSOCatalog()->getDSOName(sel.deepsky(), true);
                    if (selectionNames.find(DSOName) != string::npos && DSOName != locDSOName)
                    {
                        string::size_type startPos = selectionNames.find(locDSOName);
                        selectionNames = selectionNames.substr(startPos);
                    }
                }

                overlay->setFont(titleFont);
                *overlay << selectionNames;
                overlay->setFont(font);
                *overlay << '\n';
                displayDSOinfo(*overlay,
                               *sel.deepsky(),
                               astro::kilometersToLightYears(v.norm()) - sel.deepsky()->getRadius());
            }
            break;

        case Selection::Type_Body:
            {
                // Show all names for the body
                if (sel != lastSelection)
                {
                    lastSelection = sel;
                    selectionNames = "";
                    const vector<string>& names = sel.body()->getNames();

                    // Skip displaying the primary name if there's a localized version
                    // of the name.
                    vector<string>::const_iterator firstName = names.begin();
                    if (sel.body()->hasLocalizedName())
                        firstName++;

                    for (vector<string>::const_iterator iter = firstName; iter != names.end(); iter++)
                    {
                        if (iter != firstName)
                            selectionNames += " / ";

                        // Use localized version of parent name in alternative names.
                        string alias = *iter;
                        Selection parent = sel.parent();
                        if (parent.body() != nullptr)
                        {
                            string parentName = parent.body()->getName();
                            string locParentName = parent.body()->getName(true);
                            string::size_type startPos = alias.find(parentName);
                            if (startPos != string::npos)
                                alias.replace(startPos, parentName.length(), locParentName);
                        }

                        selectionNames += alias;
                    }
                }

                overlay->setFont(titleFont);
                *overlay << selectionNames;
                overlay->setFont(font);
                *overlay << '\n';
                displayPlanetInfo(*overlay,
                                  hudDetail,
                                  *(sel.body()),
                                  sim->getTime(),
                                  v.norm(),
                                  v);
            }
            break;

        case Selection::Type_Location:
            overlay->setFont(titleFont);
            *overlay << sel.location()->getName(true).c_str();
            overlay->setFont(font);
            *overlay << '\n';
            displayLocationInfo(*overlay,
                                *(sel.location()),
                                v.norm());
            break;

        default:
            break;
        }


        // Display RA/Dec for the selection, but only when the observer is near
        // the Earth.
        Selection refObject = sim->getFrame()->getRefObject();
        if (refObject.body() && refObject.body()->getName() == "Earth")
        {
            Body* earth = refObject.body();

            UniversalCoord observerPos = sim->getObserver().getPosition();
            double distToEarthCenter = observerPos.offsetFromKm(refObject.getPosition(sim->getTime())).norm();
            double altitude = distToEarthCenter - earth->getRadius();
            if (altitude < 1000.0)
            {
#if 1
                // Code to show the geocentric RA/Dec

                // Only show the coordinates for stars and deep sky objects, where
                // the geocentric values will match the apparent values for observers
                // near the Earth.
                if (sel.star() != nullptr || sel.deepsky() != nullptr)
                {
                    Vector3d v = sel.getPosition(sim->getTime()).offsetFromKm(Selection(earth).getPosition(sim->getTime()));
                    v = XRotation(astro::J2000Obliquity) * v;
                    displayRADec(*overlay, v);
                }
#else
                // Code to display the apparent RA/Dec for the observer

                // Don't show RA/Dec for the Earth itself
                if (sel.body() != earth)
                {
                    Vector3d vect = sel.getPosition(sim->getTime()).offsetFromKm(observerPos);
                    vect = XRotation(astro::J2000Obliquity) * vect;
                    displayRADec(*overlay, vect);
                }

                // Show the geocentric coordinates of the observer, required for
                // converting the selection RA/Dec from observer-centric to some
                // other coordinate system.
                // TODO: We should really show the planetographic (for Earth, geodetic)
                // coordinates.
                displayObserverPlanetocentricCoords(*overlay,
                                                    *earth,
                                                    observerPos,
                                                    sim->getTime());
#endif
            }
        }

        overlay->endText();

        glPopMatrix();
    }

    // Text input
    if (textEnterMode & KbAutoComplete)
    {
        overlay->setFont(titleFont);
        glPushMatrix();
        glColor4f(0.7f, 0.7f, 1.0f, 0.2f);
        overlay->rect(0.0f, 0.0f, (float) width, 100.0f);
        glTranslatef(0.0f, fontHeight * 3.0f + 35.0f, 0.0f);
        glColor4f(0.6f, 0.6f, 1.0f, 1.0f);
        overlay->beginText();
        fmt::fprintf(*overlay, _("Target name: %s"), typedText);
        overlay->endText();
        overlay->setFont(font);
        if (typedTextCompletion.size() >= 1)
        {
            int nb_cols = 4;
            int nb_lines = 3;
            int start = 0;
            glTranslatef(3.0f, -font->getHeight() - 3.0f, 0.0f);
            vector<std::string>::const_iterator iter = typedTextCompletion.begin();
            if (typedTextCompletionIdx >= nb_cols * nb_lines)
            {
               start = (typedTextCompletionIdx / nb_lines + 1 - nb_cols) * nb_lines;
               iter += start;
            }
            for (int i=0; iter < typedTextCompletion.end() && i < nb_cols; i++)
            {
                glPushMatrix();
                overlay->beginText();
                for (int j = 0; iter < typedTextCompletion.end() && j < nb_lines; iter++, j++)
                {
                    if (i * nb_lines + j == typedTextCompletionIdx - start)
                        glColor4f(1.0f, 0.6f, 0.6f, 1);
                    else
                        glColor4f(0.6f, 0.6f, 1.0f, 1);
                    *overlay << *iter << "\n";
                }
                overlay->endText();
                glPopMatrix();
                glTranslatef((float) (width/nb_cols), 0.0f, 0.0f);
           }
        }
        glPopMatrix();
        overlay->setFont(font);
    }

    // Text messages
    if (messageText != "" && currentTime < messageStart + messageDuration)
    {
        int emWidth = titleFont->getWidth("M");
        int fontHeight = titleFont->getHeight();
        int x = messageHOffset * emWidth;
        int y = messageVOffset * fontHeight;

        if (messageHOrigin == 0)
            x += width / 2;
        else if (messageHOrigin > 0)
            x += width;
        if (messageVOrigin == 0)
            y += height / 2;
        else if (messageVOrigin > 0)
            y += height;
        else if (messageVOrigin < 0)
            y -= fontHeight;

        overlay->setFont(titleFont);
        glPushMatrix();

        float alpha = 1.0f;
        if (currentTime > messageStart + messageDuration - 0.5)
            alpha = (float) ((messageStart + messageDuration - currentTime) / 0.5);
        glColor4f(textColor.red(), textColor.green(), textColor.blue(), alpha);
        glTranslatef((float) x, (float) y, 0.0f);
        overlay->beginText();
        *overlay << messageText;
        overlay->endText();
        glPopMatrix();
        overlay->setFont(font);
    }

    if (movieCapture != nullptr)
    {
        int movieWidth = movieCapture->getWidth();
        int movieHeight = movieCapture->getHeight();
        glPushMatrix();
        glColor4f(1, 0, 0, 1);
        overlay->rect((float) ((width - movieWidth) / 2 - 1),
                      (float) ((height - movieHeight) / 2 - 1),
                      (float) (movieWidth + 1),
                      (float) (movieHeight + 1), false);
        glTranslatef((float) ((width - movieWidth) / 2),
                     (float) ((height + movieHeight) / 2 + 2), 0.0f);
        overlay->beginText();
        fmt::fprintf(*overlay, _("%dx%d at %f fps  %s"),
                              movieWidth, movieHeight,
                              movieCapture->getFrameRate(),
                              recording ? _("Recording") : _("Paused"));

        overlay->endText();
        glPopMatrix();

        glPushMatrix();
        glTranslatef((float) ((width + movieWidth) / 2 - emWidth * 5),
                     (float) ((height + movieHeight) / 2 + 2),
                     0.0f);
        float sec = movieCapture->getFrameCount() /
            movieCapture->getFrameRate();
        auto min = (int) (sec / 60);
        sec -= min * 60.0f;
        overlay->beginText();
        fmt::fprintf(*overlay, "%3d:%05.2f", min, sec);
        overlay->endText();
        glPopMatrix();

        glPushMatrix();
        glTranslatef((float) ((width - movieWidth) / 2),
                     (float) ((height - movieHeight) / 2 - fontHeight - 2),
                     0.0f);
        overlay->beginText();
        *overlay << _("F11 Start/Pause    F12 Stop");
        overlay->endText();
        glPopMatrix();

        glPopMatrix();
    }

    if (editMode)
    {
        glPushMatrix();
        glTranslatef((float) ((width - font->getWidth(_("Edit Mode"))) / 2),
                     (float) (height - fontHeight), 0.0f);
        glColor4f(1, 0, 1, 1);
        *overlay << _("Edit Mode");
        glPopMatrix();
    }

    // Show logo at start
    if (logoTexture != nullptr)
    {
        glEnable(GL_TEXTURE_2D);
        if (currentTime < 5.0)
        {
            int xSize = (int) (logoTexture->getWidth() * 0.8f);
            int ySize = (int) (logoTexture->getHeight() * 0.8f);
            int left = (width - xSize) / 2;
            int bottom = height / 2;

            float topAlpha, botAlpha;
            if (currentTime < 4.0)
            {
                botAlpha = (float) clamp(currentTime / 1.0);
                topAlpha = (float) clamp(currentTime / 4.0);
            }
            else
            {
                botAlpha = topAlpha = (float) (5.0 - currentTime);
            }

            logoTexture->bind();
            glBegin(GL_QUADS);
            glColor4f(0.8f, 0.8f, 1.0f, botAlpha);
            //glColor4f(1.0f, 1.0f, 1.0f, botAlpha);
            glTexCoord2f(0.0f, 1.0f);
            glVertex2i(left, bottom);
            glTexCoord2f(1.0f, 1.0f);
            glVertex2i(left + xSize, bottom);
            glColor4f(0.6f, 0.6f, 1.0f, topAlpha);
            //glColor4f(1.0f, 1.0f, 1.0f, topAlpha);
            glTexCoord2f(1.0f, 0.0f);
            glVertex2i(left + xSize, bottom + ySize);
            glTexCoord2f(0.0f, 0.0f);
            glVertex2i(left, bottom + ySize);
            glEnd();
        }
        else
        {
            delete logoTexture;
            logoTexture = nullptr;
        }
    }

    overlay->end();
    setlocale(LC_NUMERIC, "C");
}


class SolarSystemLoader : public EnumFilesHandler
{
 public:
    Universe* universe;
    ProgressNotifier* notifier;
    SolarSystemLoader(Universe* u, ProgressNotifier* pn) : universe(u), notifier(pn) {};

    bool process(const string& filename)
    {
        if (DetermineFileType(filename) == Content_CelestiaCatalog)
        {
            string fullname = getPath() + '/' + filename;
            fmt::fprintf(clog, _("Loading solar system catalog: %s\n"), fullname);
            if (notifier != nullptr)
                notifier->update(filename);

            ifstream solarSysFile(fullname, ios::in);
            if (solarSysFile.good())
            {
                LoadSolarSystemObjects(solarSysFile,
                                       *universe,
                                       getPath());
            }
        }

        return true;
    };
};

template <class OBJDB> class CatalogLoader : public EnumFilesHandler
{
public:
    OBJDB*      objDB;
    string      typeDesc;
    ContentType contentType;
    ProgressNotifier* notifier;

    CatalogLoader(OBJDB* db,
                  const std::string& typeDesc,
                  const ContentType& contentType,
                  ProgressNotifier* pn) :
        objDB      (db),
        typeDesc   (typeDesc),
        contentType(contentType),
        notifier(pn)
    {
    }

    bool process(const string& filename)
    {
        if (DetermineFileType(filename) == contentType)
        {
            string fullname = getPath() + '/' + filename;
            fmt::fprintf(clog, _("Loading %s catalog: %s\n"), typeDesc, fullname);
            if (notifier)
                notifier->update(filename);

            ifstream catalogFile(fullname, ios::in);
            if (catalogFile.good())
            {
                bool success = objDB->load(catalogFile, getPath());
                if (!success)
                {
                    //DPRINTF(0, _("Error reading star file: %s\n"), fullname.c_str());
                    DPRINTF(0, "Error reading %s catalog file: %s\n", typeDesc.c_str(), fullname.c_str());
                }
            }
        }
        return true;
    }
};

using StarLoader = CatalogLoader<StarDatabase>;
using DeepSkyLoader = CatalogLoader<DSODatabase>;


bool CelestiaCore::initSimulation(const string& configFileName,
                                  const vector<string>& extrasDirs,
                                  ProgressNotifier* progressNotifier)
{
    if (!configFileName.empty())
    {
        config = ReadCelestiaConfig(configFileName);
    }
    else
    {
        config = ReadCelestiaConfig("celestia.cfg");

        string localConfigFile = WordExp("~/.celestia.cfg");
        if (localConfigFile != "")
            ReadCelestiaConfig(localConfigFile, config);
    }

    if (config == nullptr)
    {
        fatalError(_("Error reading configuration file."), false);
        return false;
    }

    // Set the console log size; ignore any request to use less than 100 lines
    if (config->consoleLogRows > 100)
        console.setRowCount(config->consoleLogRows);

#ifdef USE_SPICE
    if (!InitializeSpice())
    {
        fatalError(_("Initialization of SPICE library failed."), false);
        return false;
    }
#endif

    // Insert additional extras directories into the configuration. These
    // additional directories typically come from the command line. It may
    // be useful to permit other command line overrides of config file fields.
    if (!extrasDirs.empty())
    {
        // Only insert the additional extras directories that aren't also
        // listed in the configuration file. The additional directories are added
        // after the ones from the config file and the order in which they were
        // specified is preserved. This process in O(N*M), but the number of
        // additional extras directories should be small.
        for (const auto& dir : extrasDirs)
        {
            if (find(config->extrasDirs.begin(), config->extrasDirs.end(), dir) ==
                config->extrasDirs.end())
            {
                config->extrasDirs.push_back(dir);
            }
        }
    }

#ifdef CELX
    initLuaHook(progressNotifier);
#endif

    KeyRotationAccel = degToRad(config->rotateAcceleration);
    MouseRotationSensitivity = degToRad(config->mouseRotationSensitivity);

    readFavoritesFile();

    // If we couldn't read the favorites list from a file, allocate
    // an empty list.
    if (favorites == nullptr)
        favorites = new FavoritesList();

    universe = new Universe();


    /***** Load star catalogs *****/

    if (!readStars(*config, progressNotifier))
    {
        fatalError(_("Cannot read star database."), false);
        return false;
    }


    /***** Load the deep sky catalogs *****/

    DSONameDatabase* dsoNameDB  = new DSONameDatabase;
    DSODatabase*     dsoDB      = new DSODatabase;
    dsoDB->setNameDatabase(dsoNameDB);

    // Load first the vector of dsoCatalogFiles in the data directory (deepsky.dsc, globulars.dsc,...):

    for (const auto& file : config->dsoCatalogFiles)
    {
        if (progressNotifier)
            progressNotifier->update(file);

        ifstream dsoFile(file, ios::in);
        if (!dsoFile.good())
        {
            warning(fmt::sprintf(_("Error opening deepsky catalog file %s.\n"), file));
        }
        if (!dsoDB->load(dsoFile, ""))
        {
            warning(fmt::sprintf(_("Cannot read Deep Sky Objects database %s.\n"), file));
        }
    }

    // Next, read all the deep sky files in the extras directories
    {
        for (const auto& _dir : config->extrasDirs)
        {
            if (_dir != "")
            {
                Directory* dir = OpenDirectory(_dir);

                DeepSkyLoader loader(dsoDB,
                                     "deep sky object",
                                     Content_CelestiaDeepSkyCatalog,
                                     progressNotifier);
                loader.pushDir(_dir);
                dir->enumFiles(loader, true);

                delete dir;
            }
        }
    }
    dsoDB->finish();
    universe->setDSOCatalog(dsoDB);


    /***** Load the solar system catalogs *****/
    // First read the solar system files listed individually in the
    // config file.
    {
        SolarSystemCatalog* solarSystemCatalog = new SolarSystemCatalog();
        universe->setSolarSystemCatalog(solarSystemCatalog);
        for (const auto& file : config->solarSystemFiles)
        {
            if (progressNotifier)
                progressNotifier->update(file);

            ifstream solarSysFile(file, ios::in);
            if (!solarSysFile.good())
            {
                warning(fmt::sprintf(_("Error opening solar system catalog %s.\n"), file));
            }
            else
            {
                LoadSolarSystemObjects(solarSysFile, *universe, "");
            }
        }
    }

    // Next, read all the solar system files in the extras directories
    {
        for (const auto& _dir : config->extrasDirs)
        {
            if (_dir != "")
            {
                Directory* dir = OpenDirectory(_dir);

                SolarSystemLoader loader(universe, progressNotifier);
                loader.pushDir(_dir);
                dir->enumFiles(loader, true);

                delete dir;
            }
        }
    }

    // Load asterisms:
    if (config->asterismsFile != "")
    {
        ifstream asterismsFile(config->asterismsFile, ios::in);
        if (!asterismsFile.good())
        {
            warning(fmt::sprintf(_("Error opening asterisms file %s.\n"),
                                 config->asterismsFile));
        }
        else
        {
            AsterismList* asterisms = ReadAsterismList(asterismsFile,
                                                       *universe->getStarCatalog());
            universe->setAsterisms(asterisms);
        }
    }

    if (config->boundariesFile != "")
    {
        ifstream boundariesFile(config->boundariesFile, ios::in);
        if (!boundariesFile.good())
        {
            warning(fmt::sprintf(_("Error opening constellation boundaries file %s.\n"),
                                 config->boundariesFile));
        }
        else
        {
            ConstellationBoundaries* boundaries = ReadBoundaries(boundariesFile);
            universe->setBoundaries(boundaries);
        }
    }

    // Load destinations list
    if (config->destinationsFile != "")
    {
        string localeDestinationsFile = LocaleFilename(config->destinationsFile);
        ifstream destfile(localeDestinationsFile);
        if (destfile.good())
        {
            destinations = ReadDestinationList(destfile);
        }
    }

    sim = new Simulation(universe);
    if((renderer->getRenderFlags() & Renderer::ShowAutoMag) == 0)
    sim->setFaintestVisible(config->faintestVisible);

    View* view = new View(View::ViewWindow, sim->getActiveObserver(), 0.0f, 0.0f, 1.0f, 1.0f);
    views.push_back(view);
    activeView = views.begin();

    if (!compareIgnoringCase(getConfig()->cursor, "inverting crosshair"))
    {
        defaultCursorShape = CelestiaCore::InvertedCrossCursor;
    }

    if (!compareIgnoringCase(getConfig()->cursor, "arrow"))
    {
        defaultCursorShape = CelestiaCore::ArrowCursor;
    }

    if (cursorHandler != nullptr)
    {
        cursorHandler->setCursorShape(defaultCursorShape);
    }

    return true;
}


bool CelestiaCore::initRenderer()
{
    renderer->setRenderFlags(Renderer::ShowStars |
                             Renderer::ShowPlanets |
                             Renderer::ShowAtmospheres |
                             Renderer::ShowAutoMag);

#ifdef USE_GLCONTEXT
    GLContext* context = new GLContext();

    context->init(config->ignoreGLExtensions);
    // Choose the render path, starting with the least desirable
    context->setRenderPath(GLContext::GLPath_GLSL);
    //fmt::printf(_("render path: %i\n"), context->getRenderPath());
#endif

    Renderer::DetailOptions detailOptions;
    detailOptions.orbitPathSamplePoints = config->orbitPathSamplePoints;
    detailOptions.shadowTextureSize = config->shadowTextureSize;
    detailOptions.eclipseTextureSize = config->eclipseTextureSize;
    detailOptions.orbitWindowEnd = config->orbitWindowEnd;
    detailOptions.orbitPeriodsShown = config->orbitPeriodsShown;
    detailOptions.linearFadeFraction = config->linearFadeFraction;

    // Prepare the scene for rendering.
#ifdef USE_GLCONTEXT
    if (!renderer->init(context, (int) width, (int) height, detailOptions))
#else
    if (!renderer->init((int) width, (int) height, detailOptions))
#endif
    {
        fatalError(_("Failed to initialize renderer"), false);
        return false;
    }

    if ((renderer->getRenderFlags() & Renderer::ShowAutoMag) != 0)
    {
        renderer->setFaintestAM45deg(renderer->getFaintestAM45deg());
        setFaintestAutoMag();
    }

    if (config->mainFont == "")
        font = LoadTextureFont("fonts/default.txf");
    else
        font = LoadTextureFont(string("fonts/") + config->mainFont);

    if (font == nullptr)
        cout << _("Error loading font; text will not be visible.\n");
    else
        font->buildTexture();

    if (config->titleFont != "")
        titleFont = LoadTextureFont(string("fonts") + "/" + config->titleFont);
    if (titleFont != nullptr)
        titleFont->buildTexture();
    else
        titleFont = font;

    // Set up the overlay
    overlay = new Overlay();
    overlay->setWindowSize(width, height);

    if (config->labelFont == "")
    {
        renderer->setFont(Renderer::FontNormal, font);
    }
    else
    {
        TextureFont* labelFont = LoadTextureFont(string("fonts") + "/" + config->labelFont);
        if (labelFont == nullptr)
        {
            renderer->setFont(Renderer::FontNormal, font);
        }
        else
        {
            labelFont->buildTexture();
            renderer->setFont(Renderer::FontNormal, labelFont);
        }
    }

    renderer->setFont(Renderer::FontLarge, titleFont);

    if (config->logoTextureFile != "")
    {
        logoTexture = LoadTextureFromFile(string("textures") + "/" + config->logoTextureFile);
    }

    return true;
}


static void loadCrossIndex(StarDatabase* starDB,
                           StarDatabase::Catalog catalog,
                           const string& filename)
{
    if (!filename.empty())
    {
        ifstream xrefFile(filename, ios::in | ios::binary);
        if (xrefFile.good())
        {
            if (!starDB->loadCrossIndex(catalog, xrefFile))
                fmt::fprintf(cerr, _("Error reading cross index %s\n"), filename);
            else
                fmt::fprintf(clog, _("Loaded cross index %s\n"), filename);
        }
    }
}


bool CelestiaCore::readStars(const CelestiaConfig& cfg,
                             ProgressNotifier* progressNotifier)
{
    StarDetails::SetStarTextures(cfg.starTextures);

    ifstream starNamesFile(cfg.starNamesFile, ios::in);
    if (!starNamesFile.good())
    {
        fmt::fprintf(cerr, _("Error opening %s\n"), cfg.starNamesFile);
        return false;
    }

    StarNameDatabase* starNameDB = StarNameDatabase::readNames(starNamesFile);
    if (starNameDB == nullptr)
    {
        cerr << _("Error reading star names file\n");
        return false;
    }

    // First load the binary star database file.  The majority of stars
    // will be defined here.
    StarDatabase* starDB = new StarDatabase();
    if (!cfg.starDatabaseFile.empty())
    {
        if (progressNotifier)
            progressNotifier->update(cfg.starDatabaseFile);

        ifstream starFile(cfg.starDatabaseFile, ios::in | ios::binary);
        if (!starFile.good())
        {
            fmt::fprintf(cerr, _("Error opening %s\n"), cfg.starDatabaseFile);
            delete starDB;
            delete starNameDB;
            return false;
        }

        if (!starDB->loadBinary(starFile))
        {
            cerr << _("Error reading stars file\n");
            delete starDB;
            delete starNameDB;
            return false;
        }
    }

    starDB->setNameDatabase(starNameDB);

    loadCrossIndex(starDB, StarDatabase::HenryDraper, cfg.HDCrossIndexFile);
    loadCrossIndex(starDB, StarDatabase::SAO,         cfg.SAOCrossIndexFile);
    loadCrossIndex(starDB, StarDatabase::Gliese,      cfg.GlieseCrossIndexFile);

    // Next, read any ASCII star catalog files specified in the StarCatalogs
    // list.
    if (!cfg.starCatalogFiles.empty())
    {
        for (const auto& file : config->starCatalogFiles)
        {
            if (file != "")
            {
                ifstream starFile(file, ios::in);
                if (starFile.good())
                {
                    starDB->load(starFile, "");
                }
                else
                {
                    fmt::fprintf(cerr, _("Error opening star catalog %s\n"), file);
                }
            }
        }
    }

    // Now, read supplemental star files from the extras directories
    for (const auto& _dir : config->extrasDirs)
    {
        if (_dir != "")
        {
            Directory* dir = OpenDirectory(_dir);

            StarLoader loader(starDB, "star", Content_CelestiaStarCatalog, progressNotifier);
            loader.pushDir(_dir);
            dir->enumFiles(loader, true);

            delete dir;
        }
    }

    starDB->finish();

    universe->setStarCatalog(starDB);

    return true;
}


/// Set the faintest visible star magnitude; adjust the renderer's
/// brightness parameters appropriately.
void CelestiaCore::setFaintest(float magnitude)
{
    sim->setFaintestVisible(magnitude);
}

/// Set faintest visible star magnitude and saturation magnitude
/// for a given field of view;
/// adjust the renderer's brightness parameters appropriately.
void CelestiaCore::setFaintestAutoMag()
{
    float faintestMag;
    renderer->autoMag(faintestMag);
    sim->setFaintestVisible(faintestMag);
}

void CelestiaCore::fatalError(const string& msg, bool visual)
{
    if (alerter == nullptr)
        if (visual)
            flash(msg);
        else
            cerr << msg;
    else
        alerter->fatalError(msg);
}

void CelestiaCore::setAlerter(Alerter* a)
{
    alerter = a;
}

CelestiaCore::Alerter* CelestiaCore::getAlerter() const
{
    return alerter;
}

/// Sets the cursor handler object.
/// This must be set before calling initSimulation
/// or the default cursor will not be used.
void CelestiaCore::setCursorHandler(CursorHandler* handler)
{
    cursorHandler = handler;
}

CelestiaCore::CursorHandler* CelestiaCore::getCursorHandler() const
{
    return cursorHandler;
}

int CelestiaCore::getTimeZoneBias() const
{
    return timeZoneBias;
}

bool CelestiaCore::getLightDelayActive() const
{
    return lightTravelFlag;
}

void CelestiaCore::setLightDelayActive(bool lightDelayActive)
{
    lightTravelFlag = lightDelayActive;
}

void CelestiaCore::setTextEnterMode(int mode)
{
    if (mode != textEnterMode)
    {
        if ((mode & KbAutoComplete) != (textEnterMode & KbAutoComplete))
        {
            typedText = "";
            typedTextCompletion.clear();
            typedTextCompletionIdx = -1;
        }
        textEnterMode = mode;
        notifyWatchers(TextEnterModeChanged);
    }
}

int CelestiaCore::getTextEnterMode() const
{
    return textEnterMode;
}

void CelestiaCore::setScreenDpi(int dpi)
{
    screenDpi = dpi;
    setFOVFromZoom();
    renderer->setScreenDpi(dpi);
}

int CelestiaCore::getScreenDpi() const
{
    return screenDpi;
}

void CelestiaCore::setDistanceToScreen(int dts)
{
    distanceToScreen = dts;
    setFOVFromZoom();
}

int CelestiaCore::getDistanceToScreen() const
{
    return distanceToScreen;
}

void CelestiaCore::setTimeZoneBias(int bias)
{
    timeZoneBias = bias;
    notifyWatchers(TimeZoneChanged);
}


string CelestiaCore::getTimeZoneName() const
{
    return timeZoneName;
}


void CelestiaCore::setTimeZoneName(const string& zone)
{
    timeZoneName = zone;
}


int CelestiaCore::getHudDetail()
{
    return hudDetail;
}

void CelestiaCore::setHudDetail(int newHudDetail)
{
    hudDetail = newHudDetail%3;
    notifyWatchers(VerbosityLevelChanged);
}


Color CelestiaCore::getTextColor()
{
    return textColor;
}

void CelestiaCore::setTextColor(Color newTextColor)
{
    textColor = newTextColor;
}


astro::Date::Format CelestiaCore::getDateFormat() const
{
    return dateFormat;
}

void CelestiaCore::setDateFormat(astro::Date::Format format)
{
    dateStrWidth = 0;
    dateFormat = format;
}

int CelestiaCore::getOverlayElements() const
{
    return overlayElements;
}

void CelestiaCore::setOverlayElements(int _overlayElements)
{
    overlayElements = _overlayElements;
}

void CelestiaCore::initMovieCapture(MovieCapture* mc)
{
    if (movieCapture == nullptr)
        movieCapture = mc;
}

void CelestiaCore::recordBegin()
{
    if (movieCapture != nullptr) {
        recording = true;
        movieCapture->recordingStatus(true);
    }
}

void CelestiaCore::recordPause()
{
    recording = false;
    if (movieCapture != nullptr) movieCapture->recordingStatus(false);
}

void CelestiaCore::recordEnd()
{
    if (movieCapture != nullptr)
    {
        recordPause();
        movieCapture->end();
        delete movieCapture;
        movieCapture = nullptr;
    }
}

bool CelestiaCore::isCaptureActive()
{
    return movieCapture != nullptr;
}

bool CelestiaCore::isRecording()
{
    return recording;
}

void CelestiaCore::flash(const string& s, double duration)
{
    if (hudDetail > 0)
        showText(s, -1, -1, 0, 5, duration);
}


CelestiaConfig* CelestiaCore::getConfig() const
{
    return config;
}


void CelestiaCore::addWatcher(CelestiaWatcher* watcher)
{
    assert(watcher != nullptr);
    watchers.push_back(watcher);
}

void CelestiaCore::removeWatcher(CelestiaWatcher* watcher)
{
    vector<CelestiaWatcher*>::iterator iter =
        find(watchers.begin(), watchers.end(), watcher);
    if (iter != watchers.end())
        watchers.erase(iter);
}

void CelestiaCore::notifyWatchers(int property)
{
    for (const auto watcher : watchers)
    {
        watcher->notifyChange(this, property);
    }
}


void CelestiaCore::goToUrl(const string& urlStr)
{
    Url url(urlStr, this);
    url.goTo();
    notifyWatchers(RenderFlagsChanged | LabelFlagsChanged);
}


void CelestiaCore::addToHistory()
{
    Url* url = new Url(this);
    if (!history.empty() && historyCurrent < history.size() - 1)
    {
        // truncating history to current position
        while (historyCurrent != history.size() - 1)
        {
            delete history.back();
            history.pop_back();
        }
    }
    history.push_back(url);
    historyCurrent = history.size() - 1;
    notifyWatchers(HistoryChanged);
}


void CelestiaCore::back()
{
    if (historyCurrent == 0)
        return;

    if (historyCurrent == history.size() - 1)
    {
        addToHistory();
        historyCurrent = history.size()-1;
    }
    historyCurrent--;
    history[historyCurrent]->goTo();
    notifyWatchers(HistoryChanged|RenderFlagsChanged|LabelFlagsChanged);
}


void CelestiaCore::forward()
{
    if (history.size() == 0) return;
    if (historyCurrent == history.size()-1) return;
    historyCurrent++;
    history[historyCurrent]->goTo();
    notifyWatchers(HistoryChanged|RenderFlagsChanged|LabelFlagsChanged);
}


const vector<Url*>& CelestiaCore::getHistory() const
{
    return history;
}

vector<Url*>::size_type CelestiaCore::getHistoryCurrent() const
{
    return historyCurrent;
}

void CelestiaCore::setHistoryCurrent(vector<Url*>::size_type curr)
{
    if (curr >= history.size()) return;
    if (historyCurrent == history.size()) {
        addToHistory();
    }
    historyCurrent = curr;
    history[curr]->goTo();
    notifyWatchers(HistoryChanged|RenderFlagsChanged|LabelFlagsChanged);
}


/*! Toggle the specified reference mark for a selection.
 *  a selection. The default value for the selection argument is
 *  the current simulation selection. This method does nothing
 *  if the selection isn't a solar system body.
 */
void CelestiaCore::toggleReferenceMark(const string& refMark, Selection sel)
{
    Body* body = nullptr;

    if (sel.empty())
        body = getSimulation()->getSelection().body();
    else
        body = sel.body();

    // Reference marks can only be set for solar system bodies.
    if (body == nullptr)
        return;

    if (body->findReferenceMark(refMark))
    {
        body->removeReferenceMark(refMark);
    }
    else
    {
        if (refMark == "body axes")
        {
            body->addReferenceMark(new BodyAxisArrows(*body));
        }
        else if (refMark == "frame axes")
        {
            body->addReferenceMark(new FrameAxisArrows(*body));
        }
        else if (refMark == "sun direction")
        {
            body->addReferenceMark(new SunDirectionArrow(*body));
        }
        else if (refMark == "velocity vector")
        {
            body->addReferenceMark(new VelocityVectorArrow(*body));
        }
        else if (refMark == "spin vector")
        {
            body->addReferenceMark(new SpinVectorArrow(*body));
        }
        else if (refMark == "frame center direction")
        {
            double now = getSimulation()->getTime();
            BodyToBodyDirectionArrow* arrow = new BodyToBodyDirectionArrow(*body, body->getOrbitFrame(now)->getCenter());
            arrow->setTag(refMark);
            body->addReferenceMark(arrow);
        }
        else if (refMark == "planetographic grid")
        {
            body->addReferenceMark(new PlanetographicGrid(*body));
        }
        else if (refMark == "terminator")
        {
            double now = getSimulation()->getTime();
            Star* sun = nullptr;
            Body* b = body;
            while (b != nullptr)
            {
                Selection center = b->getOrbitFrame(now)->getCenter();
                if (center.star() != nullptr)
                    sun = center.star();
                b = center.body();
            }

            if (sun != nullptr)
            {
                VisibleRegion* visibleRegion = new VisibleRegion(*body, Selection(sun));
                visibleRegion->setTag("terminator");
                body->addReferenceMark(visibleRegion);
            }
        }
    }
}


/*! Return whether the specified reference mark is enabled for a
 *  a selection. The default value for the selection argument is
 *  the current simulation selection.
 */
bool CelestiaCore::referenceMarkEnabled(const string& refMark, Selection sel) const
{
    Body* body = nullptr;

    if (sel.empty())
        body = getSimulation()->getSelection().body();
    else
        body = sel.body();

    // Reference marks can only be set for solar system bodies.
    if (body == nullptr)
        return false;

    return body->findReferenceMark(refMark) != nullptr;
}


#ifdef CELX
class LuaPathFinder : public EnumFilesHandler
{
 public:
    string luaPath;
    LuaPathFinder(string s) : luaPath(s) {};
    string lastPath;

    bool process(const string& filename)
    {
        if (getPath() != lastPath)
        {
            int extPos = filename.rfind('.');
            if (extPos != (int)string::npos)
            {
                string ext = string(filename, extPos, filename.length() - extPos + 1);
                if (ext == ".lua")
                {
                    lastPath = getPath();
                    string newPatt = getPath()+"/?.lua;";
                    extPos = luaPath.rfind(newPatt);
                    if (extPos < 0)
                    {
                        luaPath = luaPath + newPatt;
                    }
                }
            }
        }
        return true;
    };
};


// Initialize the Lua hook table as well as the Lua state for scripted
// objects. The Lua hook operates in a different Lua state than user loaded
// scripts. It always has file system access via the IO package. If the script
// system access policy is "allow", then scripted objects will run in the same
// Lua context as the Lua hook. Sharing state between scripted objects and the
// hook can be very useful, but it gives system access to scripted objects,
// and therefore must be restricted based on the system access policy.
bool CelestiaCore::initLuaHook(ProgressNotifier* progressNotifier)
{
    luaHook = new LuaState();
    luaHook->init(this);

    string LuaPath = "?.lua;celxx/?.lua;";

    // Find the path for lua files in the extras directories
    {
        for (const auto& _dir : config->extrasDirs)
        {
            if (_dir != "")
            {
                Directory* dir = OpenDirectory(_dir);

                LuaPathFinder loader("");
                loader.pushDir(_dir);
                dir->enumFiles(loader, true);
                LuaPath += loader.luaPath;
                delete dir;
            }
        }
    }

    // Always grant access for the Lua hook
    luaHook->allowSystemAccess();

    luaHook->setLuaPath(LuaPath);

    int status = 0;

    // Execute the Lua hook initialization script
    if (config->luaHook != "")
    {
        string filename = config->luaHook;
        ifstream scriptfile(filename);
        if (!scriptfile.good())
        {
            string errMsg;
            errMsg = fmt::sprintf(_("Error opening LuaHook '%s'"),  filename);
            fatalError(errMsg);
        }

        if (progressNotifier)
            progressNotifier->update(config->luaHook);

        status = luaHook->loadScript(scriptfile, filename);
    }
    else
    {
        status = luaHook->loadScript("");
    }

    if (status != 0)
    {
        cerr << "lua hook load failed\n";
        string errMsg = luaHook->getErrorMessage();
        if (errMsg.empty())
            errMsg = _("Unknown error loading hook script");
        fatalError(errMsg);
        delete luaHook;
        luaHook = nullptr;
    }
    else
    {
        // Coroutine execution; control may be transferred between the
        // script and Celestia's event loop
        if (!luaHook->createThread())
        {
            cerr << "hook thread failed\n";
            string errMsg = _("Script coroutine initialization failed");
            fatalError(errMsg);
            delete luaHook;
            luaHook = nullptr;
        }

        if (luaHook)
        {
            while (!luaHook->tick(0.1)) ;
        }
    }

    // Set up the script context; if the system access policy is allow,
    // it will share the same context as the Lua hook. Otherwise, we
    // create a private context.
    if (config->scriptSystemAccessPolicy == "allow")
    {
        if (luaHook)
        {
            SetScriptedObjectContext(luaHook->getState());
        }
    }
    else
    {
        luaSandbox = new LuaState();
        luaSandbox->init(this);

        // Allow access to functions in package because we need 'require'
        // But, loadlib is prohibited.
        luaSandbox->allowLuaPackageAccess();
        luaSandbox->setLuaPath(LuaPath);

        status = luaSandbox->loadScript("");
        if (status != 0)
        {
            delete luaSandbox;
            luaSandbox = nullptr;
            return false;
        }

        SetScriptedObjectContext(luaSandbox->getState());
    }

    return true;
}
#endif

void CelestiaCore::setTypedText(const char *c_p)
{
    typedText += string(c_p);
    typedTextCompletion = sim->getObjectCompletion(typedText, (renderer->getLabelMode() & Renderer::LocationLabels) != 0);
    typedTextCompletionIdx = -1;
#ifdef AUTO_COMPLETION
    if (typedTextCompletion.size() == 1)
    {
        string::size_type pos = typedText.rfind('/', typedText.length());
        if (pos != string::npos)
            typedText = typedText.substr(0, pos + 1) + typedTextCompletion[0];
        else
            typedText = typedTextCompletion[0];
    }
#endif
}

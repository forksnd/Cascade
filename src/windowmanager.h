#ifndef WINDOWMANAGER_H
#define WINDOWMANAGER_H

#include <QObject>

class VulkanWindow;
class NodeGraph;
class PropertiesView;
class ViewerStatusBar;
class NodeBase;

enum ViewerMode
{
    VIEWER_MODE_FRONT,
    VIEWER_MODE_BACK,
    VIEWER_MODE_ALPHA,
    VIEWER_MODE_OUTPUT,
    VIEWER_MODE_NONE
};

class WindowManager : public QObject
{
    Q_OBJECT

public:
    static WindowManager& getInstance();
    WindowManager(WindowManager const&) = delete;
    void operator=(WindowManager const&) = delete;
    void setUp(VulkanWindow* vw,
               NodeGraph* ng,
               PropertiesView* pv,
               ViewerStatusBar* vb);

    ViewerMode getViewerMode();

private:
    WindowManager() {}

    bool eventFilter(QObject* watched, QEvent* event);

    VulkanWindow* vulkanWindow;
    NodeGraph* nodeGraph;
    PropertiesView* propertiesView;
    ViewerStatusBar* viewerStatusBar;

    ViewerMode currentViewerMode = VIEWER_MODE_NONE;

public slots:
    void handleNodeDoubleClicked(NodeBase* node);
    void handleZoomTextUpdateRequest(float f);
    void handleRenderTargetCreated(int w, int h);
};

#endif // WINDOWMANAGER_H
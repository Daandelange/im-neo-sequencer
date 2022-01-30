//
// Created by Matty on 2022-01-28.
//
#define IMGUI_DEFINE_MATH_OPERATORS

#include "imgui_neo_sequencer.h"
#include "imgui_internal.h"
#include "imgui_neo_internal.h"

#include <stack>

namespace ImGui {
    struct ImGuiNeoSequencerInternalData {
        ImVec2 startCursor = {0,0}; // Cursor in TL corner of whole widget
        ImVec2 topBarStartCursor = {0, 0}; // Cursor on top, below zoom slider
        ImVec2 startValuesCursor = {0, 0}; // Cursor on top of values
        ImVec2 valuesCursor = {0, 0}; // Current cursor position, used for values drawing

        ImVec2 size = {0, 0}; // Size of whole sequencer
        ImVec2 topBarSize = {0, 0}; // Size of top bar without zoom

        uint32_t startFrame = 0;
        uint32_t endFrame = 0;
        uint32_t offsetFrame = 0; // Offset from start

        float valuesWidth = 32.0f; // Width of biggest label in timeline, used for offset of timeline

        float filledHeight = 0.0f; // Height of whole sequencer

        float zoom = 1.0f;

        ImGuiID selectedTimeline = 0;

        uint32_t currentFrame = 0;
        bool holdingCurrentFrame = false; // Are we draging current frame?
        ImVec4 currentFrameColor; // Color of current frame, we have to save it because we render on EndNeoSequencer, but process at BeginneoSequencer
    };

    static ImGuiNeoSequencerStyle style; // NOLINT(cert-err58-cpp)

    //Global context stuff
    static bool inSequencer = false;

    // Height of timeline right now
    static float currentTimelineHeight = 0.0f;

    // Current active sequencer
    static ImGuiID currentSequencer;

    // Current timeline depth, used for offset of label
    static uint32_t currentTimelineDepth = 0;

    static ImVector<ImGuiColorMod> sequencerColorStack;

    // Data of all sequencers, this is main c++ part and I should create C alternative or use imgui ImVector or something
    static std::unordered_map<ImGuiID, ImGuiNeoSequencerInternalData> sequencerData;

    ///////////// STATIC HELPERS ///////////////////////

    static float getPerFrameWidth(ImGuiNeoSequencerInternalData &context) {
        return GetPerFrameWidth(context.size.x, context.valuesWidth, context.endFrame, context.startFrame,
                                context.zoom);
    }

    static float getKeyframePositionX(uint32_t frame, ImGuiNeoSequencerInternalData &context) {
        const auto perFrameWidth = getPerFrameWidth(context);
        return (frame - context.offsetFrame) * perFrameWidth;
    }

    static float getWorkTimelineWidth(ImGuiNeoSequencerInternalData &context) {
        const auto perFrameWidth = getPerFrameWidth(context);
        return context.size.x - context.valuesWidth - perFrameWidth;
    }

    // Dont pull frame from context, its used for dragging
    static ImRect getCurrentFrameBB(uint32_t frame, ImGuiNeoSequencerInternalData &context) {
        const auto &imStyle = GetStyle();
        const auto width = style.CurrentFramePointerSize * GetIO().FontGlobalScale;
        const auto cursor =
                context.topBarStartCursor + ImVec2{context.valuesWidth + imStyle.FramePadding.x - width / 2.0f, 0};
        const auto currentFrameCursor = cursor + ImVec2{getKeyframePositionX(frame, context), 0};

        float pointerHeight = style.CurrentFramePointerSize * 2.5f;
        ImRect rect{currentFrameCursor, currentFrameCursor + ImVec2{width, pointerHeight * GetIO().FontGlobalScale}};

        return rect;
    }

    static void processCurrentFrame(uint32_t *frame, ImGuiNeoSequencerInternalData &context) {
        auto pointerRect = getCurrentFrameBB(*frame, context);
        pointerRect.Min -= ImVec2{2.0f, 2.0f};
        pointerRect.Max += ImVec2{2.0f, 2.0f};

        const auto &imStyle = GetStyle();

        const auto timelineXmin = context.topBarStartCursor.x + context.valuesWidth + imStyle.FramePadding.x;

        const ImVec2 timelineXRange = {
                timelineXmin, //min
                timelineXmin + context.size.x - context.valuesWidth
        };

        if (!ItemAdd(pointerRect, 0))
            return;

        context.currentFrameColor = GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_FramePointer);

        if (IsItemHovered()) {
            context.currentFrameColor = GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_FramePointerHovered);
        }

        if (context.holdingCurrentFrame) {
            if (IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                const auto mousePosX = GetMousePos().x;
                const auto v = mousePosX - timelineXRange.x;// Subtract min

                const auto normalized = v / getWorkTimelineWidth(context); //Divide by width to remap to 0 - 1 range

                const auto clamped = ImClamp(normalized, 0.0f, 1.0f);

                const auto viewSize = (context.endFrame - context.startFrame) / context.zoom;

                const auto frameViewVal = (float) context.startFrame + (clamped * (float) viewSize);

                const auto finalFrame = (uint32_t) round(frameViewVal) + context.offsetFrame;

                context.currentFrameColor = GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_FramePointerPressed);

                *frame = finalFrame;
            }

            if (!IsMouseDown(ImGuiMouseButton_Left)) {
                context.holdingCurrentFrame = false;
                context.currentFrameColor = GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_FramePointer);
            }
        }

        if (IsItemClicked() && !context.holdingCurrentFrame) {
            context.holdingCurrentFrame = true;
            context.currentFrameColor = GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_FramePointerPressed);
        }

        context.currentFrame = *frame;
    }

    static void finishPreviousTimeline(ImGuiNeoSequencerInternalData &context) {
        context.valuesCursor = {context.topBarStartCursor.x, context.valuesCursor.y};
        currentTimelineHeight = 0.0f;
    }

    static bool createKeyframe(uint32_t *frame) {
        const auto &imStyle = GetStyle();
        auto &context = sequencerData[currentSequencer];

        const auto timelineOffset = getKeyframePositionX(*frame, context);

        const auto pos = ImVec2{context.startValuesCursor.x + imStyle.FramePadding.x, context.valuesCursor.y} +
                         ImVec2{timelineOffset + context.valuesWidth, 0};

        const auto bbPos = pos - ImVec2{currentTimelineHeight / 2, 0};

        const ImRect bb = {bbPos, bbPos + ImVec2{currentTimelineHeight, currentTimelineHeight}};

        if (!ItemAdd(bb, 0))
            return false;

        const auto drawList = ImGui::GetWindowDrawList();

        drawList->AddCircleFilled(pos + ImVec2{0, currentTimelineHeight / 2.f}, currentTimelineHeight / 3.0f,
                                  IsItemHovered() ?
                                  ColorConvertFloat4ToU32(GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_KeyframeHovered)) :
                                  ColorConvertFloat4ToU32(GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_Keyframe)), 4);

        return true;
    }


    static uint32_t idCounter = 0;
    static char		idBuffer[16];

    const char* generateID()
    {
        idBuffer[0] = '#';
        idBuffer[1] = '#';
        memset(idBuffer + 2, 0, 14);
        sprintf_s(idBuffer + 2, 14, "%o", idCounter++);

        return &idBuffer[0];
    }

    void resetID()
    {
        idCounter = 0;
    }

    static void renderCurrentFrame(ImGuiNeoSequencerInternalData &context) {
        const auto bb = getCurrentFrameBB(context.currentFrame, context);

        const auto drawList = ImGui::GetWindowDrawList();

        RenderNeoSequencerCurrentFrame(
                GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_FramePointerLine),
                context.currentFrameColor,
                bb,
                context.size.y - context.topBarSize.y,
                style.CurrentFrameLineWidth,
                drawList
        );
    }

    static void processAndRenderZoom(ImGuiNeoSequencerInternalData &context, bool allowEditingLength, uint32_t * start, uint32_t * end) {
        const auto &imStyle = GetStyle();
        ImGuiWindow *window = GetCurrentWindow();

        const auto zoomHeight = GetFontSize() * style.ZoomHeightScale;

        auto * drawList = GetWindowDrawList();

        //Input width
        const auto inputWidth = CalcTextSize("123456").x;

        const auto inputWidthWithPadding = inputWidth + imStyle.ItemSpacing.x;

        const auto cursor = allowEditingLength ? context.startCursor + ImVec2{inputWidthWithPadding, 0} : context.startCursor;

        const auto size = allowEditingLength ?
                context.size.x - 2*inputWidthWithPadding :
                context.size.x;

        const ImRect bb{cursor, cursor + ImVec2{size, zoomHeight}};

        const ImVec2 frameNumberBorderSize{inputWidth - imStyle.FramePadding.x, zoomHeight};

        const ImVec2 startFrameTextCursor{context.startCursor + ImVec2{imStyle.FramePadding.x,0}};

        // Text number borders
        //drawList->AddRect(startFrameTextCursor, startFrameTextCursor + frameNumberBorderSize,ColorConvertFloat4ToU32(GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_TimelineBorder)));

        const auto zoomBarEndWithSpacing = ImVec2{bb.Max.x + imStyle.ItemSpacing.x, context.startCursor.y};

        /*
        drawList->AddRect(zoomBarEndWithSpacing,
                          zoomBarEndWithSpacing + frameNumberBorderSize,ColorConvertFloat4ToU32(GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_TimelineBorder)));
        */

        int32_t startFrameVal = *start;
        int32_t endFrameVal = *end;

        auto prevWindowCursor = window->DC.CursorPos;

        PushItemWidth(inputWidth);
        InputScalar("##input_start_frame",ImGuiDataType_U32,&startFrameVal,NULL,NULL,NULL,allowEditingLength ? 0 : ImGuiInputTextFlags_ReadOnly );

        window->DC.CursorPos = ImVec2{zoomBarEndWithSpacing.x, prevWindowCursor.y};

        PushItemWidth(inputWidth);
        InputScalar("##input_end_frame",ImGuiDataType_U32,&endFrameVal,NULL,NULL,NULL,allowEditingLength ? 0 : ImGuiInputTextFlags_ReadOnly );

        window->DC.CursorPos = prevWindowCursor;

        if(startFrameVal < 0)
            startFrameVal = *start;

        if(endFrameVal < 0)
            endFrameVal = *end;

        if(endFrameVal < startFrameVal)
            endFrameVal = *end;

        *start = startFrameVal;
        *end = endFrameVal;

        //drawList->AddText(startFrameTextCursor + ImVec2{frameNumberBorderSize.x, 0} - ImVec2{numberTextWidth,0},IM_COL32_WHITE,numberText);

        //Background
        drawList->AddRectFilled(bb.Min, bb.Max, ColorConvertFloat4ToU32(GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_ZoomBarBg)),10.0f);

        //TODO: Add sldier scaling based on zoom and position based on frame offset

        //Render bar
        drawList->AddRectFilled(bb.Min + imStyle.ItemInnerSpacing / 2.0f, bb.Max - imStyle.ItemInnerSpacing / 2.0f, ColorConvertFloat4ToU32(GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_ZoomBarSlider)), 10.0f);
    }
    ////////////////////////////////////

    const ImVec4 &GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol idx) {
        return GetNeoSequencerStyle().Colors[idx];
    }

    ImGuiNeoSequencerStyle &GetNeoSequencerStyle() {
        return style;
    }


    bool
    BeginNeoSequencer(const char *idin, uint32_t *frame, uint32_t *startFrame, uint32_t *endFrame, const ImVec2 &size,
                      ImGuiNeoSequencerFlags flags) {
        IM_ASSERT(!inSequencer && "Called when while in other NeoSequencer, that won't work, call End!");

        ImGuiContext &g = *GImGui;
        ImGuiWindow *window = GetCurrentWindow();
        const auto &imStyle = GetStyle();
        auto &neoStyle = GetNeoSequencerStyle();

        if (inSequencer)
            return false;

        if (window->SkipItems)
            return false;

        const auto drawList = GetWindowDrawList();

        const auto cursor = GetCursorScreenPos();
        const auto area = ImGui::GetContentRegionAvail();

        const auto cursorBasePos = GetCursorScreenPos() + window->Scroll;
        const ImRect clip = {cursorBasePos, cursorBasePos + window->ContentRegionRect.GetSize()};

        PushID(idin);
        const auto id = window->IDStack[window->IDStack.size() - 1];

        inSequencer = true;

        auto &context = sequencerData[id];

        auto realSize = ImFloor(size);
        if (realSize.x <= 0.0f)
            realSize.x = ImMax(4.0f, area.x);
        if (realSize.y <= 0.0f)
            realSize.y = ImMax(4.0f, context.filledHeight);

        const bool showZoom = !(flags & ImGuiNeoSequencerFlags_HideZoom);

        context.startCursor = cursor;
        // If zoom is shown, we offset it by height of zoom bar + padding
        context.topBarStartCursor = showZoom ? cursor + ImVec2{0, GetFontSize() * style.ZoomHeightScale + imStyle.FramePadding.y } : cursor;
        context.startFrame = *startFrame;
        context.endFrame = *endFrame;
        context.size = realSize;


        currentSequencer = window->IDStack[window->IDStack.size() - 1];


        RenderNeoSequencerBackground(GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_Bg), context.startCursor,
                                     context.size,
                                     drawList, style.SequencerRounding);

        RenderNeoSequencerTopBarBackground(GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_TopBarBg),
                                           context.topBarStartCursor, context.topBarSize,
                                           drawList, style.SequencerRounding);

        RenderNeoSequencerTopBarOverlay(context.zoom, context.valuesWidth, context.startFrame, context.endFrame,
                                        context.offsetFrame,
                                        context.topBarStartCursor, context.topBarSize, drawList,
                                        style.TopBarShowFrameLines, style.TopBarShowFrameTexts);

        if(showZoom)
            processAndRenderZoom(context, flags & ImGuiNeoSequencerFlags_AllowLengthChanging, startFrame, endFrame);

        context.topBarSize = ImVec2(context.size.x, style.TopBarHeight);

        if (context.topBarSize.y <= 0.0f)
            context.topBarSize.y = CalcTextSize("100").y + imStyle.FramePadding.y * 2.0f;

        if (context.size.y < context.filledHeight)
            context.size.y = context.filledHeight;

        context.filledHeight = context.topBarSize.y + style.TopBarSpacing + (showZoom ? imStyle.FramePadding.y + style.ZoomHeightScale + GetFontSize(): 0.0f);

        context.startValuesCursor = context.topBarStartCursor + ImVec2{0, context.topBarSize.y + style.TopBarSpacing};
        context.valuesCursor = context.startValuesCursor;

        processCurrentFrame(frame, context);

        return true;
    }

    void EndNeoSequencer() {
        IM_ASSERT(inSequencer && "Called end sequencer when BeginSequencer didnt return true or wasn't called at all!");
        IM_ASSERT(sequencerData.count(currentSequencer) != 0 && "Ended sequencer has no context!");

        auto &context = sequencerData[currentSequencer];
        auto &imStyle = GetStyle();

        renderCurrentFrame(context);

        inSequencer = false;

        const ImVec2 min = {0, 0};
        context.size.y = context.filledHeight;
        const auto max = context.size;

        ItemSize({min, max});

        float viewStart = context.offsetFrame;
        const float viewWidth = context.endFrame - context.startFrame;
        const float oldGrabSize = imStyle.GrabMinSize;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        ImGui::Button("<", ImVec2(15.0f, 0.0f));
        if (ImGui::IsItemActive())
            context.zoom -= 0.0001f;
        ImGui::SameLine();
        float sliderWidth = context.size.x - 2 * 15.0f;
        ImGui::PushItemWidth(sliderWidth);
        imStyle.GrabMinSize = context.size.x / context.zoom;
        ImGui::SliderFloat("##Zoom", &viewStart, 0.0f, context.endFrame);
        context.offsetFrame = viewStart;
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::Button(">", ImVec2(15.0f, 0.0f));
        if (ImGui::IsItemActive())
            context.zoom += 0.0001f;
        ImGui::PopStyleVar();
        PopID();
        imStyle.GrabMinSize = oldGrabSize;
        resetID();
    }

    IMGUI_API bool BeginNeoGroup(const char *label, bool *open) {
        return BeginNeoTimeline(label, nullptr, 0, open, ImGuiNeoTimelineFlags_Group);
    }

    IMGUI_API void EndNeoGroup() {
        return EndNeoTimeLine();
    }

    static bool groupBehaviour(const ImGuiID id, bool* open, const ImVec2 labelSize)
    {
        auto& context = sequencerData[currentSequencer];
        ImGuiWindow* window = GetCurrentWindow();

        const bool closable = open != nullptr;

        auto drawList = ImGui::GetWindowDrawList();
        const float arrowWidth = drawList->_Data->FontSize;
        const ImVec2 arrowSize = { arrowWidth, arrowWidth };
        const ImRect arrowBB = {
                context.valuesCursor,
                context.valuesCursor + arrowSize
        };
        const ImVec2 groupBBMin = { context.valuesCursor + ImVec2{arrowSize.x, 0.0f} };
        const ImRect groupBB = {
                groupBBMin,
                groupBBMin + labelSize
        };
        const ImGuiID arrowID = window->GetID(generateID());
        const auto addArrowRes = ItemAdd(arrowBB, arrowID);
        if (addArrowRes)
        {
            if (IsItemClicked() && closable)
                (*open) = !(*open);
        }

        const auto addGroupRes = ItemAdd(groupBB, id);
        if (addGroupRes)
        {
            if (IsItemClicked())
            {
                context.selectedTimeline = context.selectedTimeline == id ? 0 : id;
            }
        }
        const float width = groupBB.Max.x - arrowBB.Min.x;
        context.valuesWidth = std::max(context.valuesWidth, width); // Make left panel wide enough
        return addGroupRes && addArrowRes;
    }

    static bool timelineBehaviour(const ImGuiID id, const ImVec2 labelSize)
    {
        auto& context = sequencerData[currentSequencer];
        ImGuiWindow* window = GetCurrentWindow();

        const ImRect groupBB = {
                context.valuesCursor,
                context.valuesCursor + labelSize
        };

        const auto addGroupRes = ItemAdd(groupBB, id);
        if (addGroupRes)
        {
            if (IsItemClicked())
            {
                context.selectedTimeline = context.selectedTimeline == id ? 0 : id;
            }
        }
        const float width = groupBB.Max.x - groupBB.Min.x;
        context.valuesWidth = std::max(context.valuesWidth, width); // Make left panel wide enough

        return addGroupRes;
    }

    bool BeginNeoTimeline(const char *label, uint32_t **keyframes, uint32_t keyframeCount, bool *open, ImGuiNeoTimelineFlags flags) {
        IM_ASSERT(inSequencer && "Not in active sequencer!");

        const bool closable = open != nullptr;

        auto &context = sequencerData[currentSequencer];
        const auto &imStyle = GetStyle();
        ImGuiWindow *window = GetCurrentWindow();
        const ImGuiID id = window->GetID(label);
        auto labelSize = CalcTextSize(label);

        labelSize.y += imStyle.FramePadding.y * 2 + style.ItemSpacing.y * 2;
        labelSize.x += imStyle.FramePadding.x * 2 + style.ItemSpacing.x * 2 +
                       (float) currentTimelineDepth * style.DepthItemSpacing;


        bool isGroup = flags & ImGuiNeoTimelineFlags_Group && closable;
        bool addRes = false;
        if (isGroup)
        {
            labelSize.x += imStyle.ItemSpacing.x + GetFontSize();
            addRes = groupBehaviour(id, open, labelSize);
        }
        else
        {
            addRes = timelineBehaviour(id, labelSize);
        }

        if (currentTimelineDepth > 0) {
            context.valuesCursor = {context.topBarStartCursor.x, context.valuesCursor.y};
        }

        currentTimelineHeight = labelSize.y;
        context.filledHeight += currentTimelineHeight;

        if (addRes)
        {
            RenderNeoTimelane(id == context.selectedTimeline,
                              context.valuesCursor + ImVec2{context.valuesWidth, 0},
                              ImVec2{context.size.x - context.valuesWidth, currentTimelineHeight},
                              GetStyleNeoSequencerColorVec4(ImGuiNeoSequencerCol_SelectedTimeline));

            ImVec4 color = GetStyleColorVec4(ImGuiCol_Text);
            if (IsItemHovered()) color.w *= 0.7f;

            RenderNeoTimelineLabel(label,
                                   context.valuesCursor + imStyle.FramePadding +
                                   ImVec2{(float)currentTimelineDepth * style.DepthItemSpacing, 0},
                                   labelSize,
                                   color,
                                   isGroup,
                                   isGroup && (*open));
        }

        for (uint32_t i = 0; i < keyframeCount; i++) {
            bool keyframeRes = createKeyframe(keyframes[i]);
        }

        context.valuesCursor.x += imStyle.FramePadding.x + (float) currentTimelineDepth * style.DepthItemSpacing;
        context.valuesCursor.y += currentTimelineHeight;

        const auto result = !closable || (*open);

        if (result)
        {
            currentTimelineDepth++;
        }
        else
        {
            finishPreviousTimeline(context);
        }
        return result;
    }

    void EndNeoTimeLine() {
        auto &context = sequencerData[currentSequencer];
        finishPreviousTimeline(context);
        currentTimelineDepth--;
    }


    bool NeoBeginCreateKeyframe(uint32_t *frame) {
        return false;
    }

#ifdef __cplusplus
    bool BeginNeoTimeline(const char *label, std::vector<uint32_t> &keyframes, bool *open) {
        std::vector<uint32_t *> c_keyframes{keyframes.size()};
        for (uint32_t i = 0; i < keyframes.size(); i++)
            c_keyframes[i] = &keyframes[i];

        return BeginNeoTimeline(label, c_keyframes.data(), c_keyframes.size(), open);
    }
#endif

    void PushNeoSequencerStyleColor(ImGuiNeoSequencerCol idx, ImU32 col) {
        ImGuiColorMod backup;
        backup.Col = idx;
        backup.BackupValue = style.Colors[idx];
        sequencerColorStack.push_back(backup);
        style.Colors[idx] = ColorConvertU32ToFloat4(col);
    }

    void PushNeoSequencerStyleColor(ImGuiNeoSequencerCol idx, const ImVec4 &col) {
        ImGuiColorMod backup;
        backup.Col = idx;
        backup.BackupValue = style.Colors[idx];
        sequencerColorStack.push_back(backup);
        style.Colors[idx] = col;
    }

    void PopNeoSequencerStyleColor(int count) {
        while (count > 0)
        {
            ImGuiColorMod& backup = sequencerColorStack.back();
            style.Colors[backup.Col] = backup.BackupValue;
            sequencerColorStack.pop_back();
            count--;
        }
    }
}

ImGuiNeoSequencerStyle::ImGuiNeoSequencerStyle() {
    Colors[ImGuiNeoSequencerCol_Bg] = ImVec4{0.31f, 0.31f, 0.31f, 1.00f};
    Colors[ImGuiNeoSequencerCol_TopBarBg] = ImVec4{0.22f, 0.22f, 0.22f, 0.84f};
    Colors[ImGuiNeoSequencerCol_SelectedTimeline] = ImVec4{0.98f, 0.706f, 0.322f, 0.88f};
    Colors[ImGuiNeoSequencerCol_TimelinesBg] = Colors[ImGuiNeoSequencerCol_TopBarBg];
    Colors[ImGuiNeoSequencerCol_TimelineBorder] = Colors[ImGuiNeoSequencerCol_Bg] * ImVec4{0.5f, 0.5f, 0.5f, 1.0f};

    Colors[ImGuiNeoSequencerCol_FramePointer] = ImVec4{0.98f, 0.24f, 0.24f, 0.50f};
    Colors[ImGuiNeoSequencerCol_FramePointerHovered] = ImVec4{0.98f, 0.15f, 0.15f, 1.00f};
    Colors[ImGuiNeoSequencerCol_FramePointerPressed] = ImVec4{0.98f, 0.08f, 0.08f, 1.00f};

    Colors[ImGuiNeoSequencerCol_Keyframe] = ImVec4{0.59f, 0.59f, 0.59f, 0.50f};
    Colors[ImGuiNeoSequencerCol_KeyframeHovered] = ImVec4{0.98f, 0.39f, 0.36f, 1.00f};
    Colors[ImGuiNeoSequencerCol_KeyframePressed] = ImVec4{0.98f, 0.39f, 0.36f, 1.00f};

    Colors[ImGuiNeoSequencerCol_FramePointerLine] = ImVec4{0.98f, 0.98f, 0.98f, 0.8f};

    Colors[ImGuiNeoSequencerCol_ZoomBarBg] = ImVec4{0.59f, 0.59f, 0.59f, 0.90f};
    Colors[ImGuiNeoSequencerCol_ZoomBarSlider] = ImVec4{0.98f, 0.98f, 0.98f, 0.60f};
    Colors[ImGuiNeoSequencerCol_ZoomBarSliderEnds] = ImVec4{0.59f, 0.59f, 0.59f, 0.90f};






    /*
        style.Colors[ImGuiCol_WindowBg] = fixColor(ImVec4(0.94f, 0.94f, 0.94f, 0.94f));
        style.Colors[ImGuiCol_ChildBg] = fixColor(ImVec4(0.00f, 0.00f, 0.00f, 0.00f));
        style.Colors[ImGuiCol_PopupBg] = fixColor(ImVec4(1.00f, 1.00f, 1.00f, 0.94f));
        style.Colors[ImGuiCol_Border] = fixColor(ImVec4(0.00f, 0.00f, 0.00f, 0.39f));
        style.Colors[ImGuiCol_BorderShadow] = fixColor(ImVec4(1.00f, 1.00f, 1.00f, 0.10f));
        style.Colors[ImGuiCol_FrameBg] = fixColor(ImVec4(1.00f, 1.00f, 1.00f, 0.94f));
        style.Colors[ImGuiCol_FrameBgHovered] = fixColor(ImVec4(0.26f, 0.59f, 0.98f, 0.40f));
        style.Colors[ImGuiCol_FrameBgActive] = fixColor(ImVec4(0.26f, 0.59f, 0.98f, 0.67f));
        style.Colors[ImGuiCol_TitleBg] = fixColor(ImVec4(0.96f, 0.96f, 0.96f, 1.00f));
        style.Colors[ImGuiCol_TitleBgCollapsed] = fixColor(ImVec4(1.00f, 1.00f, 1.00f, 0.51f));
        style.Colors[ImGuiCol_TitleBgActive] = fixColor(ImVec4(0.82f, 0.82f, 0.82f, 1.00f));
        style.Colors[ImGuiCol_MenuBarBg] = fixColor(ImVec4(0.86f, 0.86f, 0.86f, 1.00f));
        style.Colors[ImGuiCol_ScrollbarBg] = fixColor(ImVec4(0.98f, 0.98f, 0.98f, 0.53f));
        style.Colors[ImGuiCol_ScrollbarGrab] = fixColor(ImVec4(0.69f, 0.69f, 0.69f, 1.00f));
        style.Colors[ImGuiCol_ScrollbarGrabHovered] = fixColor(ImVec4(0.59f, 0.59f, 0.59f, 1.00f));
        style.Colors[ImGuiCol_ScrollbarGrabActive] = fixColor(ImVec4(0.49f, 0.49f, 0.49f, 1.00f));
        style.Colors[ImGuiCol_CheckMark] = fixColor(ImVec4(0.26f, 0.59f, 0.98f, 1.00f));
        style.Colors[ImGuiCol_SliderGrab] = fixColor(ImVec4(0.24f, 0.52f, 0.88f, 1.00f));
        style.Colors[ImGuiCol_SliderGrabActive] = fixColor(ImVec4(0.26f, 0.59f, 0.98f, 1.00f));
    */
}

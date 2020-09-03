// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "file_grid.h"
#include <set>
#include <wx/dc.h>
#include <wx/settings.h>
#include <zen/i18n.h>
#include <zen/file_error.h>
#include <zen/basic_math.h>
#include <zen/format_unit.h>
#include <zen/scope_guard.h>
#include <wx+/tooltip.h>
#include <wx+/rtl.h>
#include <wx+/dc.h>
#include <wx+/image_tools.h>
#include <wx+/image_resources.h>
#include "../base/file_hierarchy.h"

using namespace zen;
using namespace fff;


namespace fff
{
wxDEFINE_EVENT(EVENT_GRID_CHECK_ROWS,     CheckRowsEvent);
wxDEFINE_EVENT(EVENT_GRID_SYNC_DIRECTION, SyncDirectionEvent);
}


namespace
{
//let's NOT create wxWidgets objects statically:
inline wxColor getColorSyncBlue (bool faint) { if (faint) return { 0xed, 0xee, 0xff }; return { 185, 188, 255 }; }
inline wxColor getColorSyncGreen(bool faint) { if (faint) return { 0xf1, 0xff, 0xed }; return { 196, 255, 185 }; }

inline wxColor getColorConflictBackground (bool faint) { if (faint) return { 0xfe, 0xfe, 0xda }; return { 247, 252,  62 }; } //yellow
inline wxColor getColorDifferentBackground(bool faint) { if (faint) return { 0xff, 0xed, 0xee }; return { 255, 185, 187 }; } //red

inline wxColor getColorSymlinkBackground() { return { 238, 201,   0 }; } //orange
inline wxColor getColorFolderBackground () { return { 212, 208, 200 }; } //grey

inline wxColor getColorInactiveBack(bool faint) { if (faint) return { 0xf6, 0xf6, 0xf6}; return { 0xe4, 0xe4, 0xe4 }; } //light grey
inline wxColor getColorInactiveText() { return { 0x40, 0x40, 0x40 }; } //dark grey

inline wxColor getColorGridLine() { return { 192, 192, 192 }; } //light grey

const int FILE_GRID_GAP_SIZE_DIP = 2;

/* class hierarchy:            GridDataBase
                                    /|\
                     ________________|________________
                    |                                |
               GridDataRim                           |
                   /|\                               |
          __________|_________                       |
         |                    |                      |
   GridDataLeft         GridDataRight          GridDataCenter               */

std::pair<ptrdiff_t, ptrdiff_t> getVisibleRows(const Grid& grid) //returns range [from, to)
{
    const wxSize clientSize = grid.getMainWin().GetClientSize();
    if (clientSize.GetHeight() > 0)
    {
        const wxPoint topLeft = grid.CalcUnscrolledPosition(wxPoint(0, 0));
        const wxPoint bottom  = grid.CalcUnscrolledPosition(wxPoint(0, clientSize.GetHeight() - 1));

        const ptrdiff_t rowCount = grid.getRowCount();
        const ptrdiff_t rowFrom  = grid.getRowAtPos(topLeft.y); //return -1 for invalid position, rowCount if out of range
        const ptrdiff_t rowTo    = grid.getRowAtPos(bottom.y);
        if (rowFrom >= 0 && rowTo >= 0)
            return { rowFrom, std::min(rowTo + 1, rowCount) };
    }
    return {};
}


//accessibility, support high-contrast schemes => work with user-defined background color!
wxColor getAlternateBackgroundColor()
{
    const auto backCol = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

    auto incChannel = [](unsigned char c, int diff) { return static_cast<unsigned char>(std::max(0, std::min(255, c + diff))); };

    auto getAdjustedColor = [&](int diff)
    {
        return wxColor(incChannel(backCol.Red  (), diff),
                       incChannel(backCol.Green(), diff),
                       incChannel(backCol.Blue (), diff));
    };

    auto colorDist = [](const wxColor& lhs, const wxColor& rhs) //just some metric
    {
        return numeric::power<2>(static_cast<int>(lhs.Red  ()) - static_cast<int>(rhs.Red  ())) +
               numeric::power<2>(static_cast<int>(lhs.Green()) - static_cast<int>(rhs.Green())) +
               numeric::power<2>(static_cast<int>(lhs.Blue ()) - static_cast<int>(rhs.Blue ()));
    };

    const int signLevel = colorDist(backCol, *wxBLACK) < colorDist(backCol, *wxWHITE) ? 1 : -1; //brighten or darken

    //just some very faint gradient to avoid visual distraction
    const wxColor altCol = getAdjustedColor(signLevel * 10);
    return altCol;
}


//improve readability (while lacking cell borders)
wxColor getDefaultBackgroundColorAlternating(bool wantStandardColor)
{
    if (wantStandardColor)
        return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    else
        return getAlternateBackgroundColor();
}


wxColor getBackGroundColorSyncAction(SyncOperation so, bool faint)
{
    switch (so)
    {
        case SO_DO_NOTHING:
            return getColorInactiveBack(faint);
        case SO_EQUAL:
            break; //usually white

        case SO_CREATE_NEW_LEFT:
        case SO_OVERWRITE_LEFT:
        case SO_DELETE_LEFT:
        case SO_MOVE_LEFT_FROM:
        case SO_MOVE_LEFT_TO:
        case SO_COPY_METADATA_TO_LEFT:
            return getColorSyncBlue(faint);

        case SO_CREATE_NEW_RIGHT:
        case SO_OVERWRITE_RIGHT:
        case SO_DELETE_RIGHT:
        case SO_MOVE_RIGHT_FROM:
        case SO_MOVE_RIGHT_TO:
        case SO_COPY_METADATA_TO_RIGHT:
            return getColorSyncGreen(faint);

        case SO_UNRESOLVED_CONFLICT:
            return getColorConflictBackground(faint);
    }
    return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
}


wxColor getBackGroundColorCmpCategory(CompareFileResult cmpResult, bool faint)
{
    switch (cmpResult)
    {
        case FILE_LEFT_SIDE_ONLY:
        case FILE_LEFT_NEWER:
            return getColorSyncBlue(faint);

        case FILE_RIGHT_SIDE_ONLY:
        case FILE_RIGHT_NEWER:
            return getColorSyncGreen(faint);

        case FILE_DIFFERENT_CONTENT:
            return getColorDifferentBackground(faint);

        case FILE_EQUAL:
            break; //usually white

        case FILE_CONFLICT:
        case FILE_DIFFERENT_METADATA: //= sub-category of equal, but hint via background that sync direction follows conflict-setting
            return getColorConflictBackground(faint);
    }
    return wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
}


class IconUpdater;
class GridEventManager;
class GridDataLeft;
class GridDataRight;

struct IconManager
{
    IconManager(GridDataLeft& provLeft, GridDataRight& provRight, IconBuffer::IconSize sz) :
        iconBuffer_(sz),
        dirIcon_        (IconBuffer::genericDirIcon (sz)),
        linkOverlayIcon_(IconBuffer::linkOverlayIcon(sz)),
        iconUpdater_(std::make_unique<IconUpdater>(provLeft, provRight, iconBuffer_)) {}

    void startIconUpdater();
    IconBuffer& refIconBuffer() { return iconBuffer_; }

    const wxImage& getGenericDirIcon () const { return dirIcon_;         }
    const wxImage& getLinkOverlayIcon() const { return linkOverlayIcon_; }

private:
    IconBuffer iconBuffer_;
    const wxImage dirIcon_;
    const wxImage linkOverlayIcon_;

    std::unique_ptr<IconUpdater> iconUpdater_; //bind ownership to GridDataRim<>!
};


//mark rows selected on overview panel
class NavigationMarker
{
public:
    NavigationMarker() {}

    void set(std::unordered_set<const FileSystemObject*>&& markedFilesAndLinks,
             std::unordered_set<const ContainerObject*>&& markedContainer)
    {
        markedFilesAndLinks_.swap(markedFilesAndLinks);
        markedContainer_    .swap(markedContainer);
    }

    bool isMarked(const FileSystemObject& fsObj) const
    {
        if (contains(markedFilesAndLinks_, &fsObj)) //mark files/links directly
            return true;

        if (auto folder = dynamic_cast<const FolderPair*>(&fsObj))
            if (contains(markedContainer_, folder)) //mark folders which *are* the given ContainerObject*
                return true;

        //also mark all items with any matching ancestors
        for (const FileSystemObject* fsObj2 = &fsObj;;)
        {
            const ContainerObject& parent = fsObj2->parent();
            if (contains(markedContainer_, &parent))
                return true;

            fsObj2 = dynamic_cast<const FolderPair*>(&parent);
            if (!fsObj2)
                return false;
        }
    }

private:
    std::unordered_set<const FileSystemObject*> markedFilesAndLinks_; //mark files/symlinks directly within a container
    std::unordered_set<const ContainerObject*> markedContainer_;      //mark full container including all child-objects
    //DO NOT DEREFERENCE!!!! NOT GUARANTEED TO BE VALID!!!
};


struct SharedComponents //...between left, center, and right grids
{
    SharedRef<FileView> gridDataView = makeSharedRef<FileView>();
    std::unique_ptr<IconManager> iconMgr;
    NavigationMarker navMarker;
    std::unique_ptr<GridEventManager> evtMgr;
    GridViewType gridViewType = GridViewType::action;
    std::unordered_map<std::wstring, wxSize> compExtentsBuf_; //buffer expensive wxDC::GetTextExtent() calls!
};

//########################################################################################################

class GridDataBase : public GridData
{
public:
    GridDataBase(Grid& grid, const SharedRef<SharedComponents>& sharedComp) :
        grid_(grid), sharedComp_(sharedComp) {}

    void setData(FolderComparison& folderCmp)
    {
        sharedComp_.ref().gridDataView = makeSharedRef<FileView>(); //clear old data view first! avoid memory peaks!
        sharedComp_.ref().gridDataView = makeSharedRef<FileView>(folderCmp);
        sharedComp_.ref().compExtentsBuf_.clear(); //doesn't become stale! but still: re-calculate and save some memory...
    }

    GridEventManager* getEventManager() { return sharedComp_.ref().evtMgr.get(); }

    /**/  FileView& getDataView()       { return sharedComp_.ref().gridDataView.ref(); }
    const FileView& getDataView() const { return sharedComp_.ref().gridDataView.ref(); }

    void setIconManager(std::unique_ptr<IconManager> iconMgr) { sharedComp_.ref().iconMgr = std::move(iconMgr); }

    IconManager* getIconManager() { return sharedComp_.ref().iconMgr.get(); }

    GridViewType getViewType() const { return sharedComp_.ref().gridViewType; }
    void setViewType(GridViewType vt) { sharedComp_.ref().gridViewType = vt; }

    bool isNavMarked(const FileSystemObject& fsObj) const { return sharedComp_.ref().navMarker.isMarked(fsObj); }

    void setNavigationMarker(std::unordered_set<const FileSystemObject*>&& markedFilesAndLinks,
                             std::unordered_set<const ContainerObject*>&& markedContainer)
    {
        sharedComp_.ref().navMarker.set(std::move(markedFilesAndLinks), std::move(markedContainer));
    }

    Grid& refGrid() { return grid_; }
    const Grid& refGrid() const { return grid_; }

    const FileSystemObject* getFsObject(size_t row) const { return getDataView().getFsObject(row); }

    const wxSize& getTextExtentBuffered(wxDC& dc, const std::wstring& text)
    {
        auto& compExtentsBuf = sharedComp_.ref().compExtentsBuf_;
        //- only used for parent path names and file names on view => should not grow "too big"
        //- cleaned up during GridDataBase::setData()

        auto it = compExtentsBuf.find(text);
        if (it == compExtentsBuf.end())
            it = compExtentsBuf.emplace(text, dc.GetTextExtent(text)).first;
        return it->second;
    }

    int getGroupItemNamesWidth(wxDC& dc, const FileView::PathDrawInfo& pdi);

private:
    size_t getRowCount() const override { return getDataView().rowsOnView(); }

    Grid& grid_;
    SharedRef<SharedComponents> sharedComp_;
};

//########################################################################################################

template <SelectedSide side>
class GridDataRim : public GridDataBase
{
public:
    GridDataRim(Grid& grid, const SharedRef<SharedComponents>& sharedComp) : GridDataBase(grid, sharedComp) {}

    void setItemPathForm(ItemPathFormat fmt) { itemPathFormat_ = fmt; }

    void getUnbufferedIconsForPreload(std::vector<std::pair<ptrdiff_t, AbstractPath>>& newLoad) //return (priority, filepath) list
    {
        if (IconManager* iconMgr = getIconManager())
        {
            const auto& rowsOnScreen = getVisibleRows(refGrid());
            const ptrdiff_t visibleRowCount = rowsOnScreen.second - rowsOnScreen.first;

            //preload icons not yet on screen:
            const int preloadSize = 2 * std::max<ptrdiff_t>(20, visibleRowCount); //:= sum of lines above and below of visible range to preload
            //=> use full visible height to handle "next page" command and a minimum of 20 for excessive mouse wheel scrolls

            for (ptrdiff_t i = 0; i < preloadSize; ++i)
            {
                const ptrdiff_t currentRow = rowsOnScreen.first - (preloadSize + 1) / 2 + getAlternatingPos(i, visibleRowCount + preloadSize); //for odd preloadSize start one row earlier

                const IconInfo ii = getIconInfo(currentRow);
                if (ii.type == IconType::standard)
                    if (!iconMgr->refIconBuffer().readyForRetrieval(ii.fsObj->template getAbstractPath<side>()))
                        newLoad.emplace_back(i, ii.fsObj->template getAbstractPath<side>()); //insert least-important items on outer rim first
            }
        }
    }

    void updateNewAndGetUnbufferedIcons(std::vector<AbstractPath>& newLoad) //loads all not yet drawn icons
    {
        if (IconManager* iconMgr = getIconManager())
        {
            const auto& rowsOnScreen = getVisibleRows(refGrid());
            const ptrdiff_t visibleRowCount = rowsOnScreen.second - rowsOnScreen.first;

            //loop over all visible rows
            for (ptrdiff_t i = 0; i < visibleRowCount; ++i)
            {
                //alternate when adding rows: first, last, first + 1, last - 1 ...
                const ptrdiff_t currentRow = rowsOnScreen.first + getAlternatingPos(i, visibleRowCount);

                if (isFailedLoad(currentRow)) //find failed attempts to load icon
                    if (const IconInfo ii = getIconInfo(currentRow);
                        ii.type == IconType::standard)
                    {
                        //test if they are already loaded in buffer:
                        if (iconMgr->refIconBuffer().readyForRetrieval(ii.fsObj->template getAbstractPath<side>()))
                        {
                            //do a *full* refresh for *every* failed load to update partial DC updates while scrolling
                            refGrid().refreshCell(currentRow, static_cast<ColumnType>(ColumnTypeRim::path));
                            setFailedLoad(currentRow, false);
                        }
                        else //not yet in buffer: mark for async. loading
                            newLoad.push_back(ii.fsObj->template getAbstractPath<side>());
                    }
            }
        }
    }

private:
    bool isFailedLoad(size_t row) const { return row < failedLoads_.size() ? failedLoads_[row] != 0 : false; }

    void setFailedLoad(size_t row, bool failed = true)
    {
        if (failedLoads_.size() != refGrid().getRowCount())
            failedLoads_.resize(refGrid().getRowCount());

        if (row < failedLoads_.size())
            failedLoads_[row] = failed;
    }

    //icon buffer will load reversely, i.e. if we want to go from inside out, we need to start from outside in
    static size_t getAlternatingPos(size_t pos, size_t total)
    {
        assert(pos < total);
        return pos % 2 == 0 ? pos / 2 : total - 1 - pos / 2;
    }

private:
    enum class DisplayType
    {
        inactive,
        normal,
        folder,
        symlink,
    };
    DisplayType getObjectDisplayType(const FileSystemObject* fsObj) const
    {
        if (!fsObj || !fsObj->isActive())
            return DisplayType::inactive;

        DisplayType output = DisplayType::normal;

        visitFSObject(*fsObj, [&](const FolderPair& folder) { output = DisplayType::folder; },
        [](const FilePair& file) {},
        [&](const SymlinkPair& symlink) { output = DisplayType::symlink; });

        return output;
    }


    std::wstring getValue(size_t row, ColumnType colType) const override
    {
        std::wstring value;
        if (const FileSystemObject* fsObj = getFsObject(row))
            if (!fsObj->isEmpty<side>())
                switch (static_cast<ColumnTypeRim>(colType))
                {
                    case ColumnTypeRim::path:
                        switch (itemPathFormat_)
                        {
                            case ItemPathFormat::name:
                                return utfTo<std::wstring>(fsObj->getItemName<side>());
                            case ItemPathFormat::relative:
                                return utfTo<std::wstring>(fsObj->getRelativePath<side>());
                            case ItemPathFormat::full:
                                return AFS::getDisplayPath(fsObj->getAbstractPath<side>());
                        }
                        assert(false);
                        break;

                    case ColumnTypeRim::size:
                        visitFSObject(*fsObj, [&](const FolderPair& folder) { value = L"<" + _("Folder") + L">"; },
                        [&](const FilePair& file) { value = formatNumber(file.getFileSize<side>()); },
                        //[&](const FilePair& file) { value = utfTo<std::wstring>(formatAsHexString(file.getFileId<side>())); }, // -> test file id
                        [&](const SymlinkPair& symlink) { value = L"<" + _("Symlink") + L">"; });
                        break;

                    case ColumnTypeRim::date:
                        visitFSObject(*fsObj, [](const FolderPair& folder) {},
                        [&](const FilePair&       file) { value = formatUtcToLocalTime(file   .getLastWriteTime<side>()); },
                        [&](const SymlinkPair& symlink) { value = formatUtcToLocalTime(symlink.getLastWriteTime<side>()); });
                        break;

                    case ColumnTypeRim::extension:
                        visitFSObject(*fsObj, [](const FolderPair& folder) {},
                        [&](const FilePair&       file) { value = utfTo<std::wstring>(getFileExtension(file   .getItemName<side>())); },
                        [&](const SymlinkPair& symlink) { value = utfTo<std::wstring>(getFileExtension(symlink.getItemName<side>())); });
                        break;
                }
        return value;
    }

    void renderRowBackgound(wxDC& dc, const wxRect& rect, size_t row, bool enabled, bool selected) override
    {
        const FileView::PathDrawInfo pdi = getDataView().getDrawInfo(row);

        if (enabled && !selected)
        {
            const wxColor backCol = [&]
            {
                const DisplayType dispTp = getObjectDisplayType(pdi.fsObj);

                //highlight empty status by repeating middle grid colors
                if (pdi.fsObj && pdi.fsObj->isEmpty<side>())
                {
                    if (dispTp == DisplayType::inactive)
                        return getColorInactiveBack(true /*faint*/);

                    switch (getViewType())
                    {
                        case GridViewType::category:
                            return getBackGroundColorCmpCategory(pdi.fsObj->getCategory(), true /*faint*/);
                        case GridViewType::action:
                            return getBackGroundColorSyncAction(pdi.fsObj->getSyncOperation(), true /*faint*/);
                    }
                }

                if (dispTp == DisplayType::normal) //improve readability (without using cell borders)
                    return getDefaultBackgroundColorAlternating(pdi.groupIdx % 2 == 0);
#if 0
                //draw horizontal border if required
                if (const DisplayType dispTpNext = getObjectDisplayType(getFsObject(row + 1));
                    dispTp == dispTpNext)
                    drawBottomLine = true;
#endif
                switch (dispTp)
                {
                    //*INDENT-OFF*
                    case DisplayType::normal: break;
                    case DisplayType::folder:   return getColorFolderBackground();
                    case DisplayType::symlink:  return getColorSymlinkBackground();
                    case DisplayType::inactive: return getColorInactiveBack(false /*faint*/);
                    //*INDENT-ON*
                }
                assert(false);
                return wxNullColour;
            }();
            if (backCol.IsOk())
                clearArea(dc, rect, backCol);
        }
        else
            GridData::renderRowBackgound(dc, rect, row, enabled, selected);

        //----------------------------------------------------------------------------------
        wxDCPenChanger dummy(dc, wxPen(row == pdi.groupEndRow - 1 /*last group item*/ ?
                                       getColorGridLine() : getDefaultBackgroundColorAlternating(pdi.groupIdx % 2 != 0), fastFromDIP(1)));
        dc.DrawLine(rect.GetBottomLeft(), rect.GetBottomRight() + wxPoint(1, 0));
    }


    int getGroupItemNamesWidth(wxDC& dc, const FileView::PathDrawInfo& pdi)
    {
        //FileView::updateView() called? => invalidates group item render buffer
        if (pdi.viewUpdateId != viewUpdateIdLast_)
        {
            viewUpdateIdLast_ = pdi.viewUpdateId;
            groupItemNamesWidthBuf_.clear();
        }

        auto& widthBuf = groupItemNamesWidthBuf_;
        if (pdi.groupIdx >= widthBuf.size())
            widthBuf.resize(pdi.groupIdx + 1);

        int& itemNamesWidth = widthBuf[pdi.groupIdx];
        if (itemNamesWidth == 0)
        {
            itemNamesWidth = getTextExtentBuffered(dc, ELLIPSIS).x;

            std::vector<int> itemWidths;
            for (size_t row2 = pdi.groupBeginRow; row2 < pdi.groupEndRow; ++row2)
                if (const FileSystemObject* fsObj = getDataView().getFsObject(row2))
                    if (!fsObj->isEmpty<side>() && !dynamic_cast<const FolderPair*>(fsObj))
                        itemWidths.push_back(getTextExtentBuffered(dc, utfTo<std::wstring>(fsObj->getItemName<side>())).x);

            if (!itemWidths.empty())
            {
                //ignore (small number of) excess item lengths:
                auto itPercentile = itemWidths.begin() + itemWidths.size() * 8 / 10; //80th percentile
                std::nth_element(itemWidths.begin(), itPercentile, itemWidths.end()); //complexity: O(n)
                itemNamesWidth = std::max(itemNamesWidth, *itPercentile);
            }
            assert(itemNamesWidth > 0);
        }
        return itemNamesWidth;
    }


    struct GroupRenderLayout
    {
        std::wstring itemName;
        std::wstring groupName;
        std::wstring groupParentFolder;
        int iconSize;
        size_t groupBeginRow;
        bool stackedGroupRender;
        int widthGroupParent;
        int widthGroupName;
    };
    GroupRenderLayout getGroupRenderLayout(wxDC& dc, size_t row, const FileView::PathDrawInfo& pdi, int maxWidth)
    {
        assert(pdi.fsObj && pdi.folderGroupObj);

        IconManager* const iconMgr = getIconManager();
        const int iconSize = iconMgr ? iconMgr->refIconBuffer().getSize() : 0;

        //--------------------------------------------------------------------
        const int ellipsisWidth = getTextExtentBuffered(dc, ELLIPSIS).x;
        const int groupItemNamesWidth = getGroupItemNamesWidth(dc, pdi);
        //--------------------------------------------------------------------

        //exception for readability: top row is always group start!
        const size_t groupBeginRow = std::max(pdi.groupBeginRow, refGrid().getTopRow());

        const bool multiItemGroup = pdi.groupEndRow - groupBeginRow > 1;

        std::wstring itemName;
        if (!pdi.fsObj->isEmpty<side>() && !dynamic_cast<const FolderPair*>(pdi.fsObj))
            itemName = utfTo<std::wstring>(pdi.fsObj->getItemName<side>());

        std::wstring groupName;
        std::wstring groupParentFolder;
        switch (itemPathFormat_)
        {
            case ItemPathFormat::name:
                break;

            case ItemPathFormat::relative:
                if (auto groupFolder = dynamic_cast<const FolderPair*>(pdi.folderGroupObj))
                {
                    groupName         = utfTo<std::wstring>(groupFolder->template getItemName<side>());
                    groupParentFolder = utfTo<std::wstring>(groupFolder->parent().template getRelativePath<side>());
                }
                break;

            case ItemPathFormat::full:
                if (auto groupFolder = dynamic_cast<const FolderPair*>(pdi.folderGroupObj))
                {
                    groupName = utfTo<std::wstring>(groupFolder->template getItemName<side>());
                    groupParentFolder = AFS::getDisplayPath(groupFolder->parent().template getAbstractPath<side>());
                }
                else //=> BaseFolderPair
                    groupParentFolder = AFS::getDisplayPath(pdi.fsObj->base().getAbstractPath<side>());
                break;
        }
        //add slashes for better readability
        assert(!contains(groupParentFolder, L'/') || !contains(groupParentFolder, L'\\'));
        const wchar_t groupParentSep = contains(groupParentFolder, L'/') ? L'/' : (contains(groupParentFolder, L'\\') ? L'\\' : FILE_NAME_SEPARATOR);

        if (!iconMgr && !groupParentFolder.empty() &&
            !endsWith(groupParentFolder, L'/' ) && //e.g. ftp://server/
            !endsWith(groupParentFolder, L'\\'))   /*e.g  C:\ */
            groupParentFolder += groupParentSep;
        if (!iconMgr && !groupName.empty())
            groupName += FILE_NAME_SEPARATOR;

        //path components should follow the app layout direction and are NOT a single piece of text!
        //caveat: add Bidi support only during rendering and not in getValue() or AFS::getDisplayPath(): e.g. support "open file in Explorer"
        assert(!contains(groupParentFolder, slashBidi_) && !contains(groupParentFolder, bslashBidi_));
        replace(groupParentFolder, L'/',   slashBidi_);
        replace(groupParentFolder, L'\\', bslashBidi_);


        /*  group details: single row
            _______  __________________________  _______________________________________  ____________________________
            | gap |  | (group parent | (gap)) |  | ((icon | gap) | group name | (gap)) |  | (icon | gap) | item name |
            -------  --------------------------  ---------------------------------------  ----------------------------

            group details: stacked
            _______  _________________________________________________________  ____________________________
            | gap |  |   <right-aligned> ((icon | gap) | group name | (gap)) |  | (icon | gap) | item name | <- group name on first row
            -------  ---------------------------------------------------------  ----------------------------
            | gap |  | (group parent/... | gap)                              |  | (icon | gap) | item name | <- group parent on second
            -------  ---------------------------------------------------------  ----------------------------                               */
        bool stackedGroupRender = false;
        int widthGroupParent = groupParentFolder.empty() ? 0 : (getTextExtentBuffered(dc, groupParentFolder).x + (iconMgr ? gridGap_ : 0));
        int widthGroupName   = groupName        .empty() ? 0 : ((iconMgr ? iconSize + gridGap_ : 0) + getTextExtentBuffered(dc, groupName).x + (iconMgr ? gridGap_ : 0));
        int widthGroupItems  = (iconMgr ? iconSize + gridGap_ : 0) + groupItemNamesWidth;

        //not enough space? => collapse
        if (int excessWidth = gridGap_ + widthGroupParent + widthGroupName + widthGroupItems - maxWidth;
            excessWidth > 0)
        {
            if (multiItemGroup && !groupParentFolder.empty() && !groupName.empty())
            {
                //1. render group components on two rows
                stackedGroupRender = true;

                if (!endsWith(groupParentFolder, L'/' ) &&
                    !endsWith(groupParentFolder, L'\\'))
                    groupParentFolder += groupParentSep;
                groupParentFolder += ELLIPSIS;

                widthGroupParent = getTextExtentBuffered(dc, groupParentFolder).x + gridGap_;

                int widthGroupStack = std::max(widthGroupParent, widthGroupName);
                excessWidth = gridGap_ + widthGroupStack + widthGroupItems - maxWidth;

                if (excessWidth > 0)
                {
                    //2. shrink group stack (group parent only)
                    if (widthGroupParent > widthGroupName)
                    {
                        widthGroupStack = widthGroupParent = std::max(widthGroupParent - excessWidth, widthGroupName);
                        excessWidth = gridGap_ + widthGroupStack + widthGroupItems - maxWidth;
                    }
                    if (excessWidth > 0)
                    {
                        //3. shrink item rendering
                        widthGroupItems = std::max(widthGroupItems - excessWidth, (iconMgr ? iconSize + gridGap_ : 0) + ellipsisWidth);
                        excessWidth = gridGap_ + widthGroupStack + widthGroupItems - maxWidth;

                        if (excessWidth > 0)
                        {
                            //4. shrink group stack
                            widthGroupStack = std::max(widthGroupStack - excessWidth, (iconMgr ? iconSize + gridGap_ : 0) + ellipsisWidth + (iconMgr ? gridGap_ : 0));

                            widthGroupParent = std::min(widthGroupParent, widthGroupStack);
                            widthGroupName   = std::min(widthGroupName,   widthGroupStack);
                        }
                    }
                }
            }
            else //group details on single row
            {
                //1. shrink group parent
                if (!groupParentFolder.empty())
                {
                    widthGroupParent = std::max(widthGroupParent - excessWidth, ellipsisWidth + (iconMgr ? gridGap_ : 0));
                    excessWidth = gridGap_ + widthGroupParent + widthGroupName + widthGroupItems - maxWidth;
                }
                if (excessWidth > 0)
                {
                    //2. shrink item rendering
                    widthGroupItems = std::max(widthGroupItems - excessWidth, (iconMgr ? iconSize + gridGap_ : 0) + ellipsisWidth);
                    excessWidth = gridGap_ + widthGroupParent + widthGroupName + widthGroupItems - maxWidth;

                    if (excessWidth > 0)
                        //3. shrink group name
                        if (!groupName.empty())
                            widthGroupName = std::max(widthGroupName - excessWidth, (iconMgr ? iconSize + gridGap_ : 0) + ellipsisWidth + (iconMgr ? gridGap_ : 0));
                }
            }
        }

        return
        {
            itemName,
            groupName,
            groupParentFolder,
            iconSize,
            groupBeginRow,
            stackedGroupRender,
            widthGroupParent,
            widthGroupName,
        };
    }

    void renderCell(wxDC& dc, const wxRect& rect, size_t row, ColumnType colType, bool enabled, bool selected, HoverArea rowHover) override
    {
        //-----------------------------------------------
        //don't forget: harmonize with getBestSize()!!!
        //-----------------------------------------------

        wxDCTextColourChanger textColor(dc);
        if (enabled && selected) //accessibility: always set *both* foreground AND background colors!
            textColor.Set(*wxBLACK);

        if (const FileView::PathDrawInfo pdi = getDataView().getDrawInfo(row);
            pdi.fsObj)
        {
            const DisplayType dispTp = getObjectDisplayType(pdi.fsObj);

            //accessibility: always set both foreground AND background colors!
            if (enabled && !selected) //=> coordinate with renderRowBackgound()
            {
                if (dispTp == DisplayType::inactive)
                    textColor.Set(getColorInactiveText());
                else if (dispTp != DisplayType::normal)
                    textColor.Set(*wxBLACK);
            }

            wxRect rectTmp = rect;

            switch (static_cast<ColumnTypeRim>(colType))
            {
                case ColumnTypeRim::path:
                {
                    const auto& [itemName,
                                 groupName,
                                 groupParentFolder,
                                 iconSize,
                                 groupBeginRow,
                                 stackedGroupRender,
                                 widthGroupParent,
                                 widthGroupName] = getGroupRenderLayout(dc, row, pdi, rectTmp.width);

                    IconManager* const iconMgr = getIconManager();

                    auto drawIcon = [&, iconSize /*clang bug*/= iconSize](wxImage icon, wxRect rectIcon)
                    {
                        if (!pdi.fsObj->isActive())
                            icon = icon.ConvertToGreyscale(1.0 / 3, 1.0 / 3, 1.0 / 3); //treat all channels equally!

                        rectIcon.width = iconSize; //center smaller-than-default icons
                        drawBitmapRtlNoMirror(dc, icon, rectIcon, wxALIGN_CENTER);
                    };
                    //-------------------------------------------------------------------------
                    rectTmp.x     += gridGap_;
                    rectTmp.width -= gridGap_;

                    wxRect rectGroup, rectGroupParent, rectGroupName;
                    rectGroup = rectGroupParent = rectGroupName = rectTmp;

                    rectGroupParent.width = widthGroupParent;
                    rectGroupName  .width = widthGroupName;

                    if (stackedGroupRender)
                    {
                        rectGroup.width = std::max(widthGroupParent, widthGroupName);
                        rectGroupName.x += rectGroup.width - widthGroupName; //right-align
                    }
                    else //group details on single row
                    {
                        rectGroup.width = widthGroupParent + widthGroupName;
                        rectGroupName.x += widthGroupParent;
                    }
                    rectTmp.x     += rectGroup.width;
                    rectTmp.width -= rectGroup.width;

                    wxRect rectGroupItems = rectTmp;
                    //-------------------------------------------------------------------------
                    {
                        //clear background below parent path => harmonize with renderRowBackgound()
                        wxDCTextColourChanger textColorGroup(dc);
                        if (enabled && !selected &&
                            //!pdi.fsObj->isEmpty<side>() &&
                            (!groupParentFolder.empty() || !groupName.empty()) &&
                            pdi.fsObj->isActive())
                        {
                            rectGroup.x     -= gridGap_; //include lead gap
                            rectGroup.width += gridGap_; //

                            clearArea(dc, rectGroup, getDefaultBackgroundColorAlternating(pdi.groupIdx % 2 == 0));
                            //clearArea() is surprisingly expensive => call just once!
                            textColorGroup.Set(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
                            //accessibility: always set *both* foreground AND background colors!

                            if (row == pdi.groupEndRow - 1 /*last group item*/) //restore the group separation line we just cleared
                            {
                                wxDCPenChanger dummy(dc, wxPen(getColorGridLine(), fastFromDIP(1)));
                                dc.DrawLine(rectGroup.GetBottomLeft(), rectGroup.GetBottomRight() + wxPoint(1, 0));
                            }
                        }

                        if (isNavMarked(*pdi.fsObj)) //draw *after* clearing area for parent components
                        {
                            wxRect rectNav = rect;
                            rectNav.width = fastFromDIP(20);

                            wxColor backCol = *wxWHITE;
                            dc.GetPixel(rectNav.GetTopRight(), &backCol); //e.g. selected row!

                            dc.GradientFillLinear(rectNav, getColorSelectionGradientFrom(), backCol, wxEAST);
                        }

                        if (!groupName.empty() && row == groupBeginRow)
                        {
                            wxDCTextColourChanger textColorGroupName(dc);
                            if (static_cast<HoverAreaGroup>(rowHover) == HoverAreaGroup::groupName)
                            {
                                dc.GradientFillLinear(rectGroupName, getColorSelectionGradientFrom(), getColorSelectionGradientTo(), wxEAST);
                                textColorGroupName.Set(*wxBLACK);
                            }

                            if (iconMgr)
                            {
                                drawIcon(iconMgr->getGenericDirIcon(), rectGroupName);
                                rectGroupName.x     += iconSize + gridGap_;
                                rectGroupName.width -= iconSize + gridGap_;
                            }
                            drawCellText(dc, rectGroupName, groupName, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, &getTextExtentBuffered(dc, groupName));
                        }

                        if (!groupParentFolder.empty() &&
                            ((stackedGroupRender && row == groupBeginRow + 1) ||
                             (!stackedGroupRender && row == groupBeginRow)))
                        {
                            drawCellText(dc, rectGroupParent, groupParentFolder, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, &getTextExtentBuffered(dc, groupParentFolder));
                        }
                    }

                    if (!itemName.empty())
                    {
                        if (iconMgr) //draw file icon
                        {
                            //whenever there's something new to render on screen, start up watching for failed icon drawing:
                            //=> ideally it would suffice to start watching only when scrolling grid or showing new grid content, but this solution is more robust
                            //and the icon updater will stop automatically when finished anyway
                            //Note: it's not sufficient to start up on failed icon loads only, since we support prefetching of not yet visible rows!!!
                            iconMgr->startIconUpdater();

                            wxImage fileIcon;

                            const IconInfo ii = getIconInfo(row);
                            switch (ii.type)
                            {
                                case IconType::folder:
                                    fileIcon = iconMgr->getGenericDirIcon();
                                    break;

                                case IconType::standard:
                                    if (std::optional<wxImage> tmpIco = iconMgr->refIconBuffer().retrieveFileIcon(ii.fsObj->template getAbstractPath<side>()))
                                        fileIcon = *tmpIco;
                                    else
                                    {
                                        setFailedLoad(row); //save status of failed icon load -> used for async. icon loading
                                        //falsify only! we want to avoid writing incorrect success values when only partially updating the DC, e.g. when scrolling,
                                        //see repaint behavior of ::ScrollWindow() function!
                                        fileIcon = iconMgr->refIconBuffer().getIconByExtension(ii.fsObj->template getItemName<side>()); //better than nothing
                                    }
                                    break;

                                case IconType::none:
                                    break;
                            }

                            if (fileIcon.IsOk())
                            {
                                drawIcon(fileIcon, rectGroupItems);

                                if (ii.drawAsLink)
                                    drawIcon(iconMgr->getLinkOverlayIcon(), rectGroupItems);
                            }
                            rectGroupItems.x     += iconSize + gridGap_;
                            rectGroupItems.width -= iconSize + gridGap_;
                        }

                        drawCellText(dc, rectGroupItems, itemName, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, &getTextExtentBuffered(dc, itemName));
                    }
                }
                break;

                case ColumnTypeRim::size:
                    if (refGrid().GetLayoutDirection() != wxLayout_RightToLeft)
                    {
                        rectTmp.width -= gridGap_; //have file size right-justified (but don't change for RTL languages)
                        drawCellText(dc, rectTmp, getValue(row, colType), wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL);
                    }
                    else
                    {
                        rectTmp.x     += gridGap_;
                        rectTmp.width -= gridGap_;
                        drawCellText(dc, rectTmp, getValue(row, colType), wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
                    }
                    break;

                case ColumnTypeRim::date:
                case ColumnTypeRim::extension:
                    rectTmp.x     += gridGap_;
                    rectTmp.width -= gridGap_;
                    drawCellText(dc, rectTmp, getValue(row, colType), wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
                    break;
            }
        }
    }


    HoverArea getRowMouseHover(wxDC& dc, size_t row, ColumnType colType, int cellRelativePosX, int cellWidth) override
    {
        if (static_cast<ColumnTypeRim>(colType) == ColumnTypeRim::path)
            if (const FileView::PathDrawInfo pdi = getDataView().getDrawInfo(row);
                pdi.fsObj)
            {
                const auto& [itemName,
                             groupName,
                             groupParentFolder,
                             iconSize,
                             groupBeginRow,
                             stackedGroupRender,
                             widthGroupParent,
                             widthGroupName] = getGroupRenderLayout(dc, row, pdi, cellWidth);

                if (!groupName.empty() && row == groupBeginRow)
                {
                    const int groupNameCellBeginX = gridGap_ +
                                                    (stackedGroupRender ? std::max(widthGroupParent, widthGroupName) - widthGroupName : //right-align
                                                     widthGroupParent); //group details on single row

                    if (groupNameCellBeginX <= cellRelativePosX && cellRelativePosX < groupNameCellBeginX + widthGroupName)
                        return static_cast<HoverArea>(HoverAreaGroup::groupName);
                }
            }
        return HoverArea::none;
    }


    int getBestSize(wxDC& dc, size_t row, ColumnType colType) override
    {
        if (static_cast<ColumnTypeRim>(colType) == ColumnTypeRim::path)
        {
            int bestSize = 0;

            if (const FileView::PathDrawInfo pdi = getDataView().getDrawInfo(row);
                pdi.fsObj)
            {
                /* _______  __________________________  _______________________________________  ____________________________
                   | gap |  | (group parent | (gap)) |  | ((icon | gap) | group name | (gap)) |  | (icon | gap) | item name |
                   -------  --------------------------  ---------------------------------------  ----------------------------   */

                const int insanelyHugeWidth = 1000'000'000; //(hopefully) still small enough to avoid integer overflows

                const auto& [itemName,
                             groupName,
                             groupParentFolder,
                             iconSize,
                             groupBeginRow,
                             stackedGroupRender,
                             widthGroupParent,
                             widthGroupName] = getGroupRenderLayout(dc, row, pdi, insanelyHugeWidth);
                assert(!stackedGroupRender);

                const int widthGroupItem = itemName.empty() ? 0 : ((iconSize > 0 ? iconSize + gridGap_ : 0) + getTextExtentBuffered(dc, itemName).x);

                bestSize += gridGap_ + widthGroupParent + widthGroupName + widthGroupItem + gridGap_ /*[!]*/;
            }
            return bestSize;
        }
        else
        {
            const std::wstring cellValue = getValue(row, colType);
            return gridGap_ + dc.GetTextExtent(cellValue).GetWidth() + gridGap_;
        }
    }


    std::wstring getColumnLabel(ColumnType colType) const override
    {
        switch (static_cast<ColumnTypeRim>(colType))
        {
            case ColumnTypeRim::path:
                switch (itemPathFormat_)
                {
                    case ItemPathFormat::name:
                        return _("Item name");
                    case ItemPathFormat::relative:
                        return _("Relative path");
                    case ItemPathFormat::full:
                        return _("Full path");
                }
                assert(false);
                break;
            case ColumnTypeRim::size:
                return _("Size");
            case ColumnTypeRim::date:
                return _("Date");
            case ColumnTypeRim::extension:
                return _("Extension");
        }
        //assert(false); may be ColumnType::none
        return std::wstring();
    }

    void renderColumnLabel(wxDC& dc, const wxRect& rect, ColumnType colType, bool enabled, bool highlighted) override
    {
        const wxRect rectInner = drawColumnLabelBackground(dc, rect, highlighted);
        wxRect rectRemain = rectInner;

        rectRemain.x     += getColumnGapLeft();
        rectRemain.width -= getColumnGapLeft();
        drawColumnLabelText(dc, rectRemain, getColumnLabel(colType), enabled);

        //draw sort marker
        if (auto sortInfo = getDataView().getSortConfig())
            if (const ColumnTypeRim* sortType = std::get_if<ColumnTypeRim>(&sortInfo->sortCol))
                if (*sortType == static_cast<ColumnTypeRim>(colType) && sortInfo->onLeft == (side == LEFT_SIDE))
                {
                    const wxImage sortMarker = loadImage(sortInfo->ascending ? "sort_ascending" : "sort_descending");
                    drawBitmapRtlNoMirror(dc, enabled ? sortMarker : sortMarker.ConvertToDisabled(), rectInner, wxALIGN_CENTER_HORIZONTAL);
                }
    }


    std::wstring getToolTip(size_t row, ColumnType colType) const override
    {
        std::wstring toolTip;

        if (const FileSystemObject* fsObj = getFsObject(row))
            if (!fsObj->isEmpty<side>())
            {
                toolTip = getDataView().getEffectiveFolderPairCount() > 1 ?
                          AFS::getDisplayPath(fsObj->getAbstractPath<side>()) :
                          utfTo<std::wstring>(fsObj->getRelativePath<side>());

                //path components should follow the app layout direction and are NOT a single piece of text!
                //caveat: add Bidi support only during rendering and not in getValue() or AFS::getDisplayPath(): e.g. support "open file in Explorer"
                assert(!contains(toolTip, slashBidi_) && !contains(toolTip, bslashBidi_));
                replace(toolTip, L'/',   slashBidi_);
                replace(toolTip, L'\\', bslashBidi_);

                visitFSObject(*fsObj, [](const FolderPair& folder) {},
                [&](const FilePair& file)
                {
                    toolTip += L'\n' +
                               _("Size:") + L' ' + formatFilesizeShort(file.getFileSize<side>()) + L'\n' +
                               _("Date:") + L' ' + formatUtcToLocalTime(file.getLastWriteTime<side>());
                },

                [&](const SymlinkPair& symlink)
                {
                    toolTip += L'\n' +
                               _("Date:") + L' ' + formatUtcToLocalTime(symlink.getLastWriteTime<side>());
                });
            }
        return toolTip;
    }


    enum class IconType
    {
        none,
        folder,
        standard,
    };
    struct IconInfo
    {
        IconType type = IconType::none;
        const FileSystemObject* fsObj = nullptr; //only set if type != IconType::none
        bool drawAsLink = false;
    };

    IconInfo getIconInfo(size_t row) const //return ICON_FILE_FOLDER if row points to a folder
    {
        IconInfo out;

        if (const FileSystemObject* fsObj = getFsObject(row);
            fsObj && !fsObj->isEmpty<side>())
        {
            out.fsObj = fsObj;

            visitFSObject(*fsObj, [&](const FolderPair& folder)
            {
                out.type = IconType::folder;
                out.drawAsLink = folder.isFollowedSymlink<side>();
            },

            [&](const FilePair& file)
            {
                out.type       = IconType::standard;
                out.drawAsLink = file.isFollowedSymlink<side>() || hasLinkExtension(file.getItemName<side>());
            },

            [&](const SymlinkPair& symlink)
            {
                out.type       = IconType::standard;
                out.drawAsLink = true;
            });
        }
        return out;
    }

    const int gridGap_ = fastFromDIP(FILE_GRID_GAP_SIZE_DIP);

    ItemPathFormat itemPathFormat_ = ItemPathFormat::full;

    std::vector<unsigned char> failedLoads_; //effectively a vector<bool> of size "number of rows"

    const std::wstring  slashBidi_ = (wxTheApp->GetLayoutDirection() == wxLayout_RightToLeft ? RTL_MARK : LTR_MARK) + std::wstring() + L"/";
    const std::wstring bslashBidi_ = (wxTheApp->GetLayoutDirection() == wxLayout_RightToLeft ? RTL_MARK : LTR_MARK) + std::wstring() + L"\\";
    //no need for LTR/RTL marks on both sides: text follows main direction if slash is between two strong characters with different directions

    std::vector<int> groupItemNamesWidthBuf_; //buffer! groupItemNamesWidths essentially only depends on (groupIdx, side)
    uint64_t viewUpdateIdLast_ = 0;           //
};


class GridDataLeft : public GridDataRim<LEFT_SIDE>
{
public:
    GridDataLeft(Grid& grid, const SharedRef<SharedComponents>& sharedComp) : GridDataRim<LEFT_SIDE>(grid, sharedComp) {}
};

class GridDataRight : public GridDataRim<RIGHT_SIDE>
{
public:
    GridDataRight(Grid& grid, const SharedRef<SharedComponents>& sharedComp) : GridDataRim<RIGHT_SIDE>(grid, sharedComp) {}
};

//########################################################################################################

class GridDataCenter : public GridDataBase
{
public:
    GridDataCenter(Grid& grid, const SharedRef<SharedComponents>& sharedComp) : GridDataBase(grid, sharedComp),
        toolTip_(grid) {} //tool tip must not live longer than grid!

    void onSelectBegin()
    {
        selectionInProgress_ = true;
        refGrid().clearSelection(GridEventPolicy::deny); //don't emit event, prevent recursion!
        toolTip_.hide(); //handle custom tooltip
    }

    void onSelectEnd(size_t rowFirst, size_t rowLast, HoverArea rowHover, ptrdiff_t clickInitRow)
    {
        refGrid().clearSelection(GridEventPolicy::deny); //don't emit event, prevent recursion!

        //issue custom event
        if (selectionInProgress_) //don't process selections initiated by right-click
            if (rowFirst < rowLast && rowLast <= refGrid().getRowCount()) //empty? probably not in this context
                if (wxEvtHandler* evtHandler = refGrid().GetEventHandler())
                    switch (static_cast<HoverAreaCenter>(rowHover))
                    {
                        case HoverAreaCenter::checkbox:
                            if (const FileSystemObject* fsObj = getFsObject(clickInitRow))
                            {
                                const bool setIncluded = !fsObj->isActive();
                                CheckRowsEvent evt(rowFirst, rowLast, setIncluded);
                                evtHandler->ProcessEvent(evt);
                            }
                            break;
                        case HoverAreaCenter::dirLeft:
                        {
                            SyncDirectionEvent evt(rowFirst, rowLast, SyncDirection::left);
                            evtHandler->ProcessEvent(evt);
                        }
                        break;
                        case HoverAreaCenter::dirNone:
                        {
                            SyncDirectionEvent evt(rowFirst, rowLast, SyncDirection::none);
                            evtHandler->ProcessEvent(evt);
                        }
                        break;
                        case HoverAreaCenter::dirRight:
                        {
                            SyncDirectionEvent evt(rowFirst, rowLast, SyncDirection::right);
                            evtHandler->ProcessEvent(evt);
                        }
                        break;
                    }
        selectionInProgress_ = false;

        //update highlight_ and tooltip: on OS X no mouse movement event is generated after a mouse button click (unlike on Windows)
        wxPoint clientPos = refGrid().getMainWin().ScreenToClient(wxGetMousePosition());
        onMouseMovement(clientPos);
    }

    void onMouseMovement(const wxPoint& clientPos)
    {
        //manage block highlighting and custom tooltip
        if (!selectionInProgress_)
        {
            const wxPoint& topLeftAbs = refGrid().CalcUnscrolledPosition(clientPos);
            const size_t row = refGrid().getRowAtPos(topLeftAbs.y); //return -1 for invalid position, rowCount if one past the end
            const Grid::ColumnPosInfo cpi = refGrid().getColumnAtPos(topLeftAbs.x); //returns ColumnType::none if no column at x position!

            if (row < refGrid().getRowCount() && cpi.colType != ColumnType::none &&
                refGrid().getMainWin().GetClientRect().Contains(clientPos)) //cursor might have moved outside visible client area
                showToolTip(row, static_cast<ColumnTypeCenter>(cpi.colType), refGrid().getMainWin().ClientToScreen(clientPos));
            else
                toolTip_.hide();
        }
    }

    void onMouseLeave() //wxEVT_LEAVE_WINDOW does not respect mouse capture!
    {
        toolTip_.hide(); //handle custom tooltip
    }

private:
    std::wstring getValue(size_t row, ColumnType colType) const override
    {
        if (const FileSystemObject* fsObj = getFsObject(row))
            switch (static_cast<ColumnTypeCenter>(colType))
            {
                case ColumnTypeCenter::checkbox:
                    break;
                case ColumnTypeCenter::category:
                    return getSymbol(fsObj->getCategory());
                case ColumnTypeCenter::action:
                    return getSymbol(fsObj->getSyncOperation());
            }
        return std::wstring();
    }

    void renderRowBackgound(wxDC& dc, const wxRect& rect, size_t row, bool enabled, bool selected) override
    {
        const FileView::PathDrawInfo pdi = getDataView().getDrawInfo(row);

        if (enabled && !selected)
        {
            if (pdi.fsObj)
            {
                if (pdi.fsObj->isActive())
                    clearArea(dc, rect, getDefaultBackgroundColorAlternating(pdi.groupIdx % 2 == 0));
                else
                    clearArea(dc, rect, getColorInactiveBack(false /*faint*/));
            }
            else
                clearArea(dc, rect, wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        }
        else
            GridData::renderRowBackgound(dc, rect, row, enabled, selected);

        //----------------------------------------------------------------------------------
        wxDCPenChanger dummy(dc, wxPen(row == pdi.groupEndRow - 1 /*last group item*/ ?
                                       getColorGridLine() : getDefaultBackgroundColorAlternating(pdi.groupIdx % 2 != 0), fastFromDIP(1)));
        dc.DrawLine(rect.GetBottomLeft(), rect.GetBottomRight() + wxPoint(1, 0));
    }

    enum class HoverAreaCenter //each cell can be divided into four blocks concerning mouse selections
    {
        checkbox,
        dirLeft,
        dirNone,
        dirRight
    };

    void renderCell(wxDC& dc, const wxRect& rect, size_t row, ColumnType colType, bool enabled, bool selected, HoverArea rowHover) override
    {
        wxDCTextColourChanger textColor(dc);
        if (enabled && selected) //accessibility: always set *both* foreground AND background colors!
            textColor.Set(*wxBLACK);

        if (const FileView::PathDrawInfo pdi = getDataView().getDrawInfo(row);
            pdi.fsObj)
        {
            auto drawHighlightBackground = [&](const wxColor& col)
            {
                if (enabled && !selected && pdi.fsObj->isActive()) //coordinate with renderRowBackgound()!
                {
                    clearArea(dc, rect, col);

                    if (row == pdi.groupEndRow - 1 /*last group item*/) //restore the group separation line we just cleared
                    {
                        wxDCPenChanger dummy(dc, wxPen(getColorGridLine(), fastFromDIP(1)));
                        dc.DrawLine(rect.GetBottomLeft(), rect.GetBottomRight() + wxPoint(1, 0));
                    }
                }
            };

            switch (static_cast<ColumnTypeCenter>(colType))
            {
                case ColumnTypeCenter::checkbox:
                {
                    const bool drawMouseHover = static_cast<HoverAreaCenter>(rowHover) == HoverAreaCenter::checkbox;

                    if (pdi.fsObj->isActive())
                        drawBitmapRtlNoMirror(dc, loadImage(drawMouseHover ? "checkbox_true_hover" : "checkbox_true"), rect, wxALIGN_CENTER);
                    else //default
                        drawBitmapRtlNoMirror(dc, loadImage(drawMouseHover ? "checkbox_false_hover" : "checkbox_false"), rect, wxALIGN_CENTER);
                }
                break;

                case ColumnTypeCenter::category:
                {
                    if (getViewType() == GridViewType::category)
                        drawHighlightBackground(getBackGroundColorCmpCategory(pdi.fsObj->getCategory(), false /*faint*/));

                    wxRect rectTmp = rect;
                    {
                        //draw notch on left side
                        if (notch_.GetHeight() != rectTmp.height)
                            notch_.Rescale(notch_.GetWidth(), rectTmp.height);

                        //wxWidgets screws up again and has wxALIGN_RIGHT off by one pixel! -> use wxALIGN_LEFT instead
                        const wxRect rectNotch(rectTmp.x + rectTmp.width - notch_.GetWidth(), rectTmp.y, notch_.GetWidth(), rectTmp.height);
                        drawBitmapRtlNoMirror(dc, notch_, rectNotch, wxALIGN_LEFT);
                        rectTmp.width -= notch_.GetWidth();
                    }

                    if (getViewType() == GridViewType::category)
                        drawBitmapRtlMirror(dc, getCmpResultImage(pdi.fsObj->getCategory()), rectTmp, wxALIGN_CENTER, renderBufCmp_);
                    else if (pdi.fsObj->getCategory() != FILE_EQUAL) //don't show = in both middle columns
                        drawBitmapRtlMirror(dc, greyScale(getCmpResultImage(pdi.fsObj->getCategory())), rectTmp, wxALIGN_CENTER, renderBufCmp_);
                }
                break;

                case ColumnTypeCenter::action:
                {
                    if (getViewType() == GridViewType::action)
                        drawHighlightBackground(getBackGroundColorSyncAction(pdi.fsObj->getSyncOperation(), false /*faint*/));

                    //synchronization preview
                    const auto rowHoverCenter = rowHover == HoverArea::none ? HoverAreaCenter::checkbox : static_cast<HoverAreaCenter>(rowHover);
                    switch (rowHoverCenter)
                    {
                        case HoverAreaCenter::dirLeft:
                            drawBitmapRtlMirror(dc, getSyncOpImage(pdi.fsObj->testSyncOperation(SyncDirection::left)), rect, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, renderBufSync_);
                            break;
                        case HoverAreaCenter::dirNone:
                            drawBitmapRtlNoMirror(dc, getSyncOpImage(pdi.fsObj->testSyncOperation(SyncDirection::none)), rect, wxALIGN_CENTER);
                            break;
                        case HoverAreaCenter::dirRight:
                            drawBitmapRtlMirror(dc, getSyncOpImage(pdi.fsObj->testSyncOperation(SyncDirection::right)), rect, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL, renderBufSync_);
                            break;
                        case HoverAreaCenter::checkbox:
                            if (getViewType() == GridViewType::action)
                                drawBitmapRtlMirror(dc, getSyncOpImage(pdi.fsObj->getSyncOperation()), rect, wxALIGN_CENTER, renderBufSync_);
                            else if (pdi.fsObj->getSyncOperation() != SO_EQUAL) //don't show = in both middle columns
                                drawBitmapRtlMirror(dc, greyScale(getSyncOpImage(pdi.fsObj->getSyncOperation())), rect, wxALIGN_CENTER, renderBufSync_);
                            break;
                    }
                }
                break;
            }
        }
    }

    HoverArea getRowMouseHover(wxDC& dc, size_t row, ColumnType colType, int cellRelativePosX, int cellWidth) override
    {
        if (const FileSystemObject* const fsObj = getFsObject(row))
            switch (static_cast<ColumnTypeCenter>(colType))
            {
                case ColumnTypeCenter::checkbox:
                case ColumnTypeCenter::category:
                    return static_cast<HoverArea>(HoverAreaCenter::checkbox);

                case ColumnTypeCenter::action:
                    if (fsObj->getSyncOperation() == SO_EQUAL) //in sync-preview equal files shall be treated like a checkbox
                        return static_cast<HoverArea>(HoverAreaCenter::checkbox);
                    /* cell: ------------------------
                             | left | middle | right|
                             ------------------------    */
                    if (0 <= cellRelativePosX)
                    {
                        if (cellRelativePosX < cellWidth / 3)
                            return static_cast<HoverArea>(HoverAreaCenter::dirLeft);
                        else if (cellRelativePosX < 2 * cellWidth / 3)
                            return static_cast<HoverArea>(HoverAreaCenter::dirNone);
                        else if  (cellRelativePosX < cellWidth)
                            return static_cast<HoverArea>(HoverAreaCenter::dirRight);
                    }
                    break;
            }
        return HoverArea::none;
    }

    std::wstring getColumnLabel(ColumnType colType) const override
    {
        switch (static_cast<ColumnTypeCenter>(colType))
        {
            case ColumnTypeCenter::checkbox:
                break;
            case ColumnTypeCenter::category:
                return _("Category") + L" (F11)";
            case ColumnTypeCenter::action:
                return _("Action")   + L" (F11)";
        }
        return std::wstring();
    }

    std::wstring getToolTip(ColumnType colType) const override { return getColumnLabel(colType); }

    void renderColumnLabel(wxDC& dc, const wxRect& rect, ColumnType colType, bool enabled, bool highlighted) override
    {
        const auto colTypeCenter = static_cast<ColumnTypeCenter>(colType);

        const wxRect rectInner = drawColumnLabelBackground(dc, rect, highlighted && colTypeCenter != ColumnTypeCenter::checkbox);

        wxImage colIcon;
        switch (colTypeCenter)
        {
            case ColumnTypeCenter::checkbox:
                break;

            case ColumnTypeCenter::category:
                colIcon = greyScaleIfDisabled(loadImage("compare_sicon"), getViewType() == GridViewType::category);
                break;

            case ColumnTypeCenter::action:
                colIcon = greyScaleIfDisabled(loadImage("file_sync_sicon"), getViewType() == GridViewType::action);
                break;
        }

        if (colIcon.IsOk())
            drawBitmapRtlNoMirror(dc, enabled ? colIcon : colIcon.ConvertToDisabled(), rectInner, wxALIGN_CENTER);

        //draw sort marker
        if (auto sortInfo = getDataView().getSortConfig())
            if (const ColumnTypeCenter* sortType = std::get_if<ColumnTypeCenter>(&sortInfo->sortCol))
                if (*sortType == colTypeCenter)
                {
                    const int gapLeft = (rectInner.width + colIcon.GetWidth()) / 2;
                    wxRect rectRemain = rectInner;
                    rectRemain.x     += gapLeft;
                    rectRemain.width -= gapLeft;

                    const wxImage sortMarker = loadImage(sortInfo->ascending ? "sort_ascending" : "sort_descending");
                    drawBitmapRtlNoMirror(dc, enabled ? sortMarker : sortMarker.ConvertToDisabled(), rectRemain, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
                }
    }

    void showToolTip(size_t row, ColumnTypeCenter colType, wxPoint posScreen)
    {
        if (const FileSystemObject* fsObj = getFsObject(row))
        {
            switch (colType)
            {
                case ColumnTypeCenter::checkbox:
                case ColumnTypeCenter::category:
                {
                    const char* imageName = [&]
                    {
                        const CompareFileResult cmpRes = fsObj->getCategory();
                        switch (cmpRes)
                            {
                            //*INDENT-OFF*
                            case FILE_LEFT_SIDE_ONLY:     return "cat_left_only";
                            case FILE_RIGHT_SIDE_ONLY:    return "cat_right_only";
                            case FILE_LEFT_NEWER:         return "cat_left_newer";
                            case FILE_RIGHT_NEWER:        return "cat_right_newer";
                            case FILE_DIFFERENT_CONTENT:  return "cat_different";
                            case FILE_EQUAL:
                            case FILE_DIFFERENT_METADATA: return "cat_equal"; //= sub-category of equal
case FILE_CONFLICT:           return "cat_conflict";
//*INDENT-ON*
                        }
                        assert(false);
                        return "";
                    }
                    ();
                    const auto& img = mirrorIfRtl(loadImage(imageName));
                    toolTip_.show(getCategoryDescription(*fsObj), posScreen, &img);
                }
                break;

            case ColumnTypeCenter::action:
            {
                const char* imageName = [&]
                {
                    const SyncOperation syncOp = fsObj->getSyncOperation();
                    switch (syncOp)
                            {
                            //*INDENT-OFF*
                            case SO_CREATE_NEW_LEFT:        return "so_create_left";
                            case SO_CREATE_NEW_RIGHT:       return "so_create_right";
                            case SO_DELETE_LEFT:            return "so_delete_left";
                            case SO_DELETE_RIGHT:           return "so_delete_right";
                            case SO_MOVE_LEFT_FROM:         return "so_move_left_source";
                            case SO_MOVE_LEFT_TO:           return "so_move_left_target";
                            case SO_MOVE_RIGHT_FROM:        return "so_move_right_source";
                            case SO_MOVE_RIGHT_TO:          return "so_move_right_target";
                            case SO_OVERWRITE_LEFT:         return "so_update_left";
                            case SO_OVERWRITE_RIGHT:        return "so_update_right";
                            case SO_COPY_METADATA_TO_LEFT:  return "so_move_left";
                            case SO_COPY_METADATA_TO_RIGHT: return "so_move_right";
                            case SO_DO_NOTHING:             return "so_none";
                            case SO_EQUAL:                  return "cat_equal";
case SO_UNRESOLVED_CONFLICT:    return "cat_conflict";
//*INDENT-ON*
                    };
                    assert(false);
                    return "";
                }();
                const auto& img = mirrorIfRtl(loadImage(imageName));
                toolTip_.show(getSyncOpDescription(*fsObj), posScreen, &img);
            }
            break;
    }
}
else
    toolTip_.hide(); //if invalid row...
    }

    bool selectionInProgress_ = false;

    std::optional<wxBitmap> renderBufCmp_; //avoid costs of recreating this temporary variable
    std::optional<wxBitmap> renderBufSync_;
    Tooltip toolTip_;
    wxImage notch_ = loadImage("notch");
};

//########################################################################################################

wxDEFINE_EVENT(EVENT_ALIGN_SCROLLBARS, wxCommandEvent);


class GridEventManager : private wxEvtHandler
{
public:
    GridEventManager(Grid& gridL,
             Grid& gridC,
             Grid& gridR,
             GridDataCenter& provCenter) :
gridL_(gridL), gridC_(gridC), gridR_(gridR),
provCenter_(provCenter)
    {
gridL_.Bind(EVENT_GRID_COL_RESIZE, [this](GridColumnResizeEvent& event) { onResizeColumnL(event); });
gridR_.Bind(EVENT_GRID_COL_RESIZE, [this](GridColumnResizeEvent& event) { onResizeColumnR(event); });

gridL_.getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onKeyDown(event, gridL_); });
gridC_.getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onKeyDown(event, gridC_); });
gridR_.getMainWin().Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) { onKeyDown(event, gridR_); });

gridC_.getMainWin().Bind(wxEVT_MOTION,       [this](wxMouseEvent& event) { onCenterMouseMovement(event); });
gridC_.getMainWin().Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) { onCenterMouseLeave   (event); });

gridC_.Bind(EVENT_GRID_MOUSE_LEFT_DOWN, [this](GridClickEvent&  event) { onCenterSelectBegin(event); });
gridC_.Bind(EVENT_GRID_SELECT_RANGE,    [this](GridSelectEvent& event) { onCenterSelectEnd  (event); });

//clear selection of other grid when selecting on
gridL_.Bind(EVENT_GRID_SELECT_RANGE, [this](GridSelectEvent& event) { onGridSelectionL(event); });
gridR_.Bind(EVENT_GRID_SELECT_RANGE, [this](GridSelectEvent& event) { onGridSelectionR(event); });

//parallel grid scrolling: do NOT use DoPrepareDC() to align grids! GDI resource leak! Use regular paint event instead:
gridL_.getMainWin().Bind(wxEVT_PAINT, [this](wxPaintEvent& event) { onPaintGrid(gridL_); event.Skip(); });
gridC_.getMainWin().Bind(wxEVT_PAINT, [this](wxPaintEvent& event) { onPaintGrid(gridC_); event.Skip(); });
gridR_.getMainWin().Bind(wxEVT_PAINT, [this](wxPaintEvent& event) { onPaintGrid(gridR_); event.Skip(); });

auto connectGridAccess = [&](Grid& grid, std::function<void(wxEvent& event)> handler)
{
    grid.Bind(wxEVT_SCROLLWIN_TOP,        handler);
    grid.Bind(wxEVT_SCROLLWIN_BOTTOM,     handler);
    grid.Bind(wxEVT_SCROLLWIN_LINEUP,     handler);
    grid.Bind(wxEVT_SCROLLWIN_LINEDOWN,   handler);
    grid.Bind(wxEVT_SCROLLWIN_PAGEUP,     handler);
    grid.Bind(wxEVT_SCROLLWIN_PAGEDOWN,   handler);
    grid.Bind(wxEVT_SCROLLWIN_THUMBTRACK, handler);
    //wxEVT_KILL_FOCUS -> there's no need to reset "scrollMaster"
    //wxEVT_SET_FOCUS -> not good enough:
    //e.g.: left grid has input, right grid is "scrollMaster" due to dragging scroll thumb via mouse.
    //=> Next keyboard input on left does *not* emit focus change event, but still "scrollMaster" needs to change
    //=> hook keyboard input instead of focus event:
    grid.getMainWin().Bind(wxEVT_CHAR,     handler);
    grid.getMainWin().Bind(wxEVT_KEY_UP,   handler);
    grid.getMainWin().Bind(wxEVT_KEY_DOWN, handler);

    grid.getMainWin().Bind(wxEVT_LEFT_DOWN,   handler);
    grid.getMainWin().Bind(wxEVT_LEFT_DCLICK, handler);
    grid.getMainWin().Bind(wxEVT_RIGHT_DOWN,  handler);
    grid.getMainWin().Bind(wxEVT_MOUSEWHEEL,  handler);
};
connectGridAccess(gridL_, [this](wxEvent& event) { onGridAccessL(event); }); //
connectGridAccess(gridC_, [this](wxEvent& event) { onGridAccessC(event); }); //connect *after* onKeyDown() in order to receive callback *before*!!!
connectGridAccess(gridR_, [this](wxEvent& event) { onGridAccessR(event); }); //

Bind(EVENT_ALIGN_SCROLLBARS, [this](wxCommandEvent& event) { onAlignScrollBars(event); });
    }

    ~GridEventManager()
    {
//assert(!scrollbarUpdatePending_); => false-positives: e.g. start ffs, right-click on grid, close dialog by clicking X
    }

    void setScrollMaster(const Grid& grid) { scrollMaster_ = &grid; }

private:
    void onCenterSelectBegin(GridClickEvent& event)
    {
provCenter_.onSelectBegin();
event.Skip();
    }

    void onCenterSelectEnd(GridSelectEvent& event)
    {
if (event.positive_)
{
    if (event.mouseClick_)
        provCenter_.onSelectEnd(event.rowFirst_, event.rowLast_, event.mouseClick_->hoverArea_, event.mouseClick_->row_);
    else
        provCenter_.onSelectEnd(event.rowFirst_, event.rowLast_, HoverArea::none, -1);
}
event.Skip();
    }

    void onCenterMouseMovement(wxMouseEvent& event)
    {
provCenter_.onMouseMovement(event.GetPosition());
event.Skip();
    }

    void onCenterMouseLeave(wxMouseEvent& event)
    {
provCenter_.onMouseLeave();
event.Skip();
    }

    void onGridSelectionL(GridSelectEvent& event) { onGridSelection(gridL_, gridR_); event.Skip(); }
    void onGridSelectionR(GridSelectEvent& event) { onGridSelection(gridR_, gridL_); event.Skip(); }

    void onGridSelection(const Grid& grid, Grid& other)
    {
if (!wxGetKeyState(WXK_CONTROL)) //clear other grid unless user is holding CTRL
    other.clearSelection(GridEventPolicy::deny); //don't emit event, prevent recursion!
    }

    void onKeyDown(wxKeyEvent& event, const Grid& grid)
    {
int keyCode = event.GetKeyCode();
if (grid.GetLayoutDirection() == wxLayout_RightToLeft)
{
    if (keyCode == WXK_LEFT || keyCode == WXK_NUMPAD_LEFT)
        keyCode = WXK_RIGHT;
    else if (keyCode == WXK_RIGHT || keyCode == WXK_NUMPAD_RIGHT)
        keyCode = WXK_LEFT;
}

//skip middle component when navigating via keyboard
const size_t row = grid.getGridCursor();

if (event.ShiftDown())
    ;
else if (event.ControlDown())
    ;
else
    switch (keyCode)
    {
        case WXK_LEFT:
        case WXK_NUMPAD_LEFT:
            gridL_.setGridCursor(row, GridEventPolicy::allow);
            gridL_.SetFocus();
            //since key event is likely originating from right grid, we need to set scrollMaster manually!
            scrollMaster_ = &gridL_; //onKeyDown is called *after* onGridAccessL()!
            return; //swallow event

        case WXK_RIGHT:
        case WXK_NUMPAD_RIGHT:
            gridR_.setGridCursor(row, GridEventPolicy::allow);
            gridR_.SetFocus();
            scrollMaster_ = &gridR_;
            return; //swallow event
    }

event.Skip();
    }

    void onResizeColumnL(GridColumnResizeEvent& event) { resizeOtherSide(gridL_, gridR_, event.colType_, event.offset_); }
    void onResizeColumnR(GridColumnResizeEvent& event) { resizeOtherSide(gridR_, gridL_, event.colType_, event.offset_); }

    void resizeOtherSide(const Grid& src, Grid& trg, ColumnType type, int offset)
    {
//find stretch factor of resized column: type is unique due to makeConsistent()!
std::vector<Grid::ColAttributes> cfgSrc = src.getColumnConfig();
auto it = std::find_if(cfgSrc.begin(), cfgSrc.end(), [&](Grid::ColAttributes& ca) { return ca.type == type; });
if (it == cfgSrc.end())
    return;
const int stretchSrc = it->stretch;

//we do not propagate resizings on stretched columns to the other side: awkward user experience
if (stretchSrc > 0)
    return;

//apply resized offset to other side, but only if stretch factors match!
std::vector<Grid::ColAttributes> cfgTrg = trg.getColumnConfig();
for (Grid::ColAttributes& ca : cfgTrg)
    if (ca.type == type && ca.stretch == stretchSrc)
        ca.offset = offset;
trg.setColumnConfig(cfgTrg);
    }

    void onGridAccessL(wxEvent& event) { scrollMaster_ = &gridL_; event.Skip(); }
    void onGridAccessC(wxEvent& event) { scrollMaster_ = &gridC_; event.Skip(); }
    void onGridAccessR(wxEvent& event) { scrollMaster_ = &gridR_; event.Skip(); }

    void onPaintGrid(const Grid& grid)
    {
//align scroll positions of all three grids *synchronously* during paint event! (wxGTK has visible delay when this is done asynchronously, no delay on Windows)

//determine lead grid
const Grid* lead = nullptr;
Grid* follow1    = nullptr;
Grid* follow2    = nullptr;
auto setGrids = [&](const Grid& l, Grid& f1, Grid& f2) { lead = &l; follow1 = &f1; follow2 = &f2; };

if (&gridC_ == scrollMaster_)
    setGrids(gridC_, gridL_, gridR_);
else if (&gridR_ == scrollMaster_)
    setGrids(gridR_, gridL_, gridC_);
else //default: left panel
    setGrids(gridL_, gridC_, gridR_);

//align other grids only while repainting the lead grid to avoid scrolling and updating a grid at the same time!
if (lead == &grid)
{
    auto scroll = [](Grid& target, int y) //support polling
    {
        //scroll vertically only - scrolling horizontally becomes annoying if left and right sides have different widths;
        //e.g. h-scroll on left would be undone when scrolling vertically on right which doesn't have a h-scrollbar
        int yOld = 0;
        target.GetViewStart(nullptr, &yOld);
        if (yOld != y)
            target.Scroll(-1, y); //empirical test Windows/Ubuntu: this call does NOT trigger a wxEVT_SCROLLWIN event, which would incorrectly set "scrollMaster" to "&target"!
        //CAVEAT: wxScrolledWindow::Scroll() internally calls wxWindow::Update(), leading to immediate WM_PAINT handling in the target grid!
        //        an this while we're still in our WM_PAINT handler! => no recursion, fine (hopefully)
    };
    int y = 0;
    lead->GetViewStart(nullptr, &y);
    scroll(*follow1, y);
    scroll(*follow2, y);
}

//harmonize placement of horizontal scrollbar to avoid grids getting out of sync!
//since this affects the grid that is currently repainted as well, we do work asynchronously!
if (!scrollbarUpdatePending_) //send one async event at most, else they may accumulate and create perf issues, see grid.cpp
{
    scrollbarUpdatePending_ = true;
    wxCommandEvent alignEvent(EVENT_ALIGN_SCROLLBARS);
    AddPendingEvent(alignEvent); //waits until next idle event - may take up to a second if the app is busy on wxGTK!
}
    }

    void onAlignScrollBars(wxEvent& event)
    {
assert(scrollbarUpdatePending_);
ZEN_ON_SCOPE_EXIT(scrollbarUpdatePending_ = false);

auto needsHorizontalScrollbars = [](const Grid& grid) -> bool
{
    const wxWindow& mainWin = grid.getMainWin();
    return mainWin.GetVirtualSize().GetWidth() > mainWin.GetClientSize().GetWidth();
    //assuming Grid::updateWindowSizes() does its job well, this should suffice!
    //CAVEAT: if horizontal and vertical scrollbar are circular dependent from each other
    //(h-scrollbar is shown due to v-scrollbar consuming horizontal width, etc...)
    //while in fact both are NOT needed, this special case results in a bogus need for scrollbars!
    //see https://sourceforge.net/tracker/?func=detail&aid=3514183&group_id=234430&atid=1093083
    // => since we're outside the Grid abstraction, we should not duplicate code to handle this special case as it seems to be insignificant
};

Grid::ScrollBarStatus sbStatusX = needsHorizontalScrollbars(gridL_) ||
                                  needsHorizontalScrollbars(gridR_) ?
                                  Grid::SB_SHOW_ALWAYS : Grid::SB_SHOW_NEVER;
gridL_.showScrollBars(sbStatusX, Grid::SB_SHOW_NEVER);
gridC_.showScrollBars(sbStatusX, Grid::SB_SHOW_NEVER);
gridR_.showScrollBars(sbStatusX, Grid::SB_SHOW_AUTOMATIC);
    }

    Grid& gridL_;
    Grid& gridC_;
    Grid& gridR_;

    const Grid* scrollMaster_ = nullptr; //for address check only; this needn't be the grid having focus!
    //e.g. mouse wheel events should set window under cursor as scrollMaster, but *not* change focus

    GridDataCenter& provCenter_;
    bool scrollbarUpdatePending_ = false;
};
}

//########################################################################################################

void filegrid::init(Grid& gridLeft, Grid& gridCenter, Grid& gridRight)
{
    auto sharedComp = makeSharedRef<SharedComponents>();

    auto provLeft_   = std::make_shared<GridDataLeft  >(gridLeft,   sharedComp);
    auto provCenter_ = std::make_shared<GridDataCenter>(gridCenter, sharedComp);
    auto provRight_  = std::make_shared<GridDataRight >(gridRight,  sharedComp);

    sharedComp.ref().evtMgr = std::make_unique<GridEventManager>(gridLeft, gridCenter, gridRight, *provCenter_);

    gridLeft  .setDataProvider(provLeft_);   //data providers reference grid =>
    gridCenter.setDataProvider(provCenter_); //ownership must belong *exclusively* to grid!
    gridRight .setDataProvider(provRight_);

    gridCenter.enableColumnMove  (false);
    gridCenter.enableColumnResize(false);

    gridCenter.showRowLabel(false);
    gridRight .showRowLabel(false);

    //gridLeft  .showScrollBars(Grid::SB_SHOW_AUTOMATIC, Grid::SB_SHOW_NEVER); -> redundant: configuration happens in GridEventManager::onAlignScrollBars()
    //gridCenter.showScrollBars(Grid::SB_SHOW_NEVER,     Grid::SB_SHOW_NEVER);

    const int widthCheckbox =     loadImage("checkbox_true").GetWidth() + fastFromDIP(3);
    const int widthCategory = 2 * loadImage("sort_ascending").GetWidth() + loadImage("cat_left_only_sicon").GetWidth() + loadImage("notch").GetWidth();
    const int widthAction   = 3 * loadImage("so_create_left_sicon").GetWidth();
    gridCenter.SetSize(widthCategory + widthCheckbox + widthAction, -1);

    gridCenter.setColumnConfig(
    {
{ static_cast<ColumnType>(ColumnTypeCenter::checkbox), widthCheckbox, 0, true },
{ static_cast<ColumnType>(ColumnTypeCenter::category), widthCategory, 0, true },
{ static_cast<ColumnType>(ColumnTypeCenter::action),   widthAction,   0, true },
    });
}


void filegrid::setData(Grid& grid, FolderComparison& folderCmp)
{
    if (auto* prov = dynamic_cast<GridDataBase*>(grid.getDataProvider()))
return prov->setData(folderCmp);

    throw std::runtime_error("filegrid was not initialized! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));
}


FileView& filegrid::getDataView(Grid& grid)
{
    if (auto* prov = dynamic_cast<GridDataBase*>(grid.getDataProvider()))
return prov->getDataView();

    throw std::runtime_error("filegrid was not initialized! " + std::string(__FILE__) + ':' + numberTo<std::string>(__LINE__));
}


namespace
{
class IconUpdater : private wxEvtHandler //update file icons periodically: use SINGLE instance to coordinate left and right grids in parallel
{
public:
    IconUpdater(GridDataLeft& provLeft, GridDataRight& provRight, IconBuffer& iconBuffer) : provLeft_(provLeft), provRight_(provRight), iconBuffer_(iconBuffer)
    {
timer_.Bind(wxEVT_TIMER, [this](wxTimerEvent& event) { loadIconsAsynchronously(event); });
    }

    void start() { if (!timer_.IsRunning()) timer_.Start(100); } //timer interval in [ms]
    //don't check too often! give worker thread some time to fetch data

private:
    void stop() { if (timer_.IsRunning()) timer_.Stop(); }

    void loadIconsAsynchronously(wxEvent& event) //loads all (not yet) drawn icons
    {
std::vector<std::pair<ptrdiff_t, AbstractPath>> prefetchLoad;
provLeft_ .getUnbufferedIconsForPreload(prefetchLoad);
provRight_.getUnbufferedIconsForPreload(prefetchLoad);

//make sure least-important prefetch rows are inserted first into workload (=> processed last)
//priority index nicely considers both grids at the same time!
std::sort(prefetchLoad.begin(), prefetchLoad.end(), [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });

//last inserted items are processed first in icon buffer:
std::vector<AbstractPath> newLoad;
for (const auto& [priority, filePath] : prefetchLoad)
    newLoad.push_back(filePath);

provRight_.updateNewAndGetUnbufferedIcons(newLoad);
provLeft_ .updateNewAndGetUnbufferedIcons(newLoad);

iconBuffer_.setWorkload(newLoad);

if (newLoad.empty()) //let's only pay for IconUpdater while needed
    stop();
    }

    GridDataLeft&  provLeft_;
    GridDataRight& provRight_;
    IconBuffer& iconBuffer_;
    wxTimer timer_;
};


//resolve circular linker dependencies
inline
void IconManager::startIconUpdater() { if (iconUpdater_) iconUpdater_->start(); }
}


void filegrid::setupIcons(Grid& gridLeft, Grid& gridCenter, Grid& gridRight, bool show, IconBuffer::IconSize sz)
{
    auto* provLeft  = dynamic_cast<GridDataLeft*>(gridLeft .getDataProvider());
    auto* provRight = dynamic_cast<GridDataRight*>(gridRight.getDataProvider());

    if (provLeft && provRight)
    {
int iconHeight = 0;
if (show)
{
    auto iconMgr = std::make_unique<IconManager>(*provLeft, *provRight, sz);
    iconHeight = iconMgr->refIconBuffer().getSize();
    provLeft ->setIconManager(std::move(iconMgr));
}
else
{
    iconHeight = IconBuffer::getSize(IconBuffer::SIZE_SMALL);
    provLeft ->setIconManager(nullptr);
}

const int newRowHeight = std::max(iconHeight, gridLeft.getMainWin().GetCharHeight()) + fastFromDIP(1); //add some space

gridLeft  .setRowHeight(newRowHeight);
gridCenter.setRowHeight(newRowHeight);
gridRight .setRowHeight(newRowHeight);
    }
    else
assert(false);
}


void filegrid::setItemPathForm(Grid& grid, ItemPathFormat fmt)
{
    if (auto* provLeft  = dynamic_cast<GridDataLeft*>(grid.getDataProvider()))
provLeft->setItemPathForm(fmt);
    else if (auto* provRight = dynamic_cast<GridDataRight*>(grid.getDataProvider()))
provRight->setItemPathForm(fmt);
    else
assert(false);
    grid.Refresh();
}


void filegrid::refresh(Grid& gridLeft, Grid& gridCenter, Grid& gridRight)
{
    gridLeft  .Refresh();
    gridCenter.Refresh();
    gridRight .Refresh();
}


void filegrid::setScrollMaster(Grid& grid)
{
    if (auto prov = dynamic_cast<GridDataBase*>(grid.getDataProvider()))
if (auto evtMgr = prov->getEventManager())
{
    evtMgr->setScrollMaster(grid);
    return;
}
    assert(false);
}


void filegrid::setNavigationMarker(Grid& gridLeft,
                           zen::Grid& gridRight,
                           std::unordered_set<const FileSystemObject*>&& markedFilesAndLinks,
                           std::unordered_set<const ContainerObject*>&& markedContainer)
{
    if (auto grid = dynamic_cast<GridDataBase*>(gridLeft.getDataProvider()))
grid->setNavigationMarker(std::move(markedFilesAndLinks), std::move(markedContainer));
    else
assert(false);
    gridLeft .Refresh();
    gridRight.Refresh();
}


void filegrid::setViewType(Grid& gridCenter, GridViewType vt)
{
    if (auto prov = dynamic_cast<GridDataBase*>(gridCenter.getDataProvider()))
prov->setViewType(vt);
    else
assert(false);
    gridCenter.Refresh();
}


wxImage fff::getSyncOpImage(SyncOperation syncOp)
{
    switch (syncOp) //evaluate comparison result and sync direction
    {
        //*INDENT-OFF*
        case SO_CREATE_NEW_LEFT:        return loadImage("so_create_left_sicon");
        case SO_CREATE_NEW_RIGHT:       return loadImage("so_create_right_sicon");
        case SO_DELETE_LEFT:            return loadImage("so_delete_left_sicon");
        case SO_DELETE_RIGHT:           return loadImage("so_delete_right_sicon");
        case SO_MOVE_LEFT_FROM:         return loadImage("so_move_left_source_sicon");
        case SO_MOVE_LEFT_TO:           return loadImage("so_move_left_target_sicon");
        case SO_MOVE_RIGHT_FROM:        return loadImage("so_move_right_source_sicon");
        case SO_MOVE_RIGHT_TO:          return loadImage("so_move_right_target_sicon");
        case SO_OVERWRITE_LEFT:         return loadImage("so_update_left_sicon");
        case SO_OVERWRITE_RIGHT:        return loadImage("so_update_right_sicon");
        case SO_COPY_METADATA_TO_LEFT:  return loadImage("so_move_left_sicon");
        case SO_COPY_METADATA_TO_RIGHT: return loadImage("so_move_right_sicon");
        case SO_DO_NOTHING:             return loadImage("so_none_sicon");
        case SO_EQUAL:                  return loadImage("cat_equal_sicon");
case SO_UNRESOLVED_CONFLICT:    return loadImage("cat_conflict_small");
//*INDENT-ON*
    }
    assert(false);
    return wxNullImage;
}


wxImage fff::getCmpResultImage(CompareFileResult cmpResult)
{
    switch (cmpResult)
    {
        //*INDENT-OFF*
        case FILE_LEFT_SIDE_ONLY:     return loadImage("cat_left_only_sicon");
        case FILE_RIGHT_SIDE_ONLY:    return loadImage("cat_right_only_sicon");
        case FILE_LEFT_NEWER:         return loadImage("cat_left_newer_sicon");
        case FILE_RIGHT_NEWER:        return loadImage("cat_right_newer_sicon");
        case FILE_DIFFERENT_CONTENT:  return loadImage("cat_different_sicon");
        case FILE_EQUAL: 
        case FILE_DIFFERENT_METADATA: return loadImage("cat_equal_sicon"); //= sub-category of equal
case FILE_CONFLICT:           return loadImage("cat_conflict_small");
//*INDENT-ON*
    }
    assert(false);
    return wxNullImage;
}

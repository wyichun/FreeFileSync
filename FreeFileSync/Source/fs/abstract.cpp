// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "abstract.h"
#include <zen/serialize.h>
#include <zen/guid.h>
#include <zen/crc.h>

using namespace zen;
using namespace fff;
using AFS = AbstractFileSystem;


const Zchar* AFS::TEMP_FILE_ENDING = Zstr(".ffs_tmp");


bool fff::isValidRelPath(const Zstring& relPath)
{
    return !contains(relPath, '\\') &&
           !startsWith(relPath, FILE_NAME_SEPARATOR) && !endsWith(relPath, FILE_NAME_SEPARATOR) &&
           !contains(relPath, Zstring() + FILE_NAME_SEPARATOR + FILE_NAME_SEPARATOR);
}


int AFS::compareAbstractPath(const AbstractPath& lhs, const AbstractPath& rhs)
{
    //note: in worst case, order is guaranteed to be stable only during each program run
    if (typeid(*lhs.afs).before(typeid(*rhs.afs)))
        return -1;
    if (typeid(*rhs.afs).before(typeid(*lhs.afs)))
        return 1;
    assert(typeid(*lhs.afs) == typeid(*rhs.afs));
    //caveat: typeid returns static type for pointers, dynamic type for references!!!

    const int rv = lhs.afs->compareDeviceRootSameAfsType(*rhs.afs);
    if (rv != 0)
        return rv;

    return compareFilePath(lhs.afsPath.value, rhs.afsPath.value);
}


std::optional<AbstractPath> AFS::getParentFolderPath(const AbstractPath& ap)
{
    if (const std::optional<AfsPath> parentAfsPath = getParentAfsPath(ap.afsPath))
        return AbstractPath(ap.afs, *parentAfsPath);

    return {};
}


std::optional<AfsPath> AFS::getParentAfsPath(const AfsPath& afsPath)
{
    if (afsPath.value.empty())
        return {};

    return AfsPath(beforeLast(afsPath.value, FILE_NAME_SEPARATOR, IF_MISSING_RETURN_NONE));
}


void AFS::traverseFolderRecursive(const AbstractPath& basePath, const AFS::TraverserWorkload& workload, size_t parallelOps)
{
    TraverserWorkloadImpl wlImpl;
    for (const auto& [relPathComponents, cb] : workload)
    {
        AfsPath afsPath = basePath.afsPath;
        for (const Zstring& itemName : relPathComponents)
        {
            assert(!contains(itemName, FILE_NAME_SEPARATOR));
            if (!afsPath.value.empty())
                afsPath.value += FILE_NAME_SEPARATOR;
            afsPath.value += itemName;
        }
        wlImpl.emplace_back(afsPath, cb);
    }
    basePath.afs->traverseFolderRecursive(wlImpl, parallelOps); //throw
}


namespace
{
struct FlatTraverserCallback : public AFS::TraverserCallback
{
    FlatTraverserCallback(const std::function<void (const AFS::FileInfo&    fi)>& onFile,
                          const std::function<void (const AFS::FolderInfo&  fi)>& onFolder,
                          const std::function<void (const AFS::SymlinkInfo& si)>& onSymlink) :
        onFile_   (onFile),
        onFolder_ (onFolder),
        onSymlink_(onSymlink) {}

private:
    void                               onFile   (const AFS::FileInfo&    fi) override { if (onFile_)    onFile_   (fi); }
    std::shared_ptr<TraverserCallback> onFolder (const AFS::FolderInfo&  fi) override { if (onFolder_)  onFolder_ (fi); return nullptr; }
    HandleLink                         onSymlink(const AFS::SymlinkInfo& si) override { if (onSymlink_) onSymlink_(si); return TraverserCallback::LINK_SKIP; }

    HandleError reportDirError (const std::wstring& msg, size_t retryNumber)                          override { throw FileError(msg); }
    HandleError reportItemError(const std::wstring& msg, size_t retryNumber, const Zstring& itemName) override { throw FileError(msg); }

    const std::function<void (const AFS::FileInfo&    fi)> onFile_;
    const std::function<void (const AFS::FolderInfo&  fi)> onFolder_;
    const std::function<void (const AFS::SymlinkInfo& si)> onSymlink_;
};
}


void AFS::traverseFolderFlat(const AfsPath& afsPath, //throw FileError
                             const std::function<void (const FileInfo&    fi)>& onFile,
                             const std::function<void (const FolderInfo&  fi)>& onFolder,
                             const std::function<void (const SymlinkInfo& si)>& onSymlink) const
{
    auto ft = std::make_shared<FlatTraverserCallback>(onFile, onFolder, onSymlink); //throw FileError
    traverseFolderRecursive({{ afsPath, ft }}, 1 /*parallelOps*/); //throw FileError
}


//target existing: undefined behavior! (fail/overwrite/auto-rename)
AFS::FileCopyResult AFS::copyFileAsStream(const AfsPath& afsPathSource, const StreamAttributes& attrSource, //throw FileError, ErrorFileLocked
                                          const AbstractPath& apTarget, const IOCallback& notifyUnbufferedIO) const
{
    int64_t totalUnbufferedIO = 0;

    auto streamIn = getInputStream(afsPathSource, IOCallbackDivider(notifyUnbufferedIO, totalUnbufferedIO)); //throw FileError, ErrorFileLocked, X

    StreamAttributes attrSourceNew = {};
    //try to get the most current attributes if possible (input file might have changed after comparison!)
    if (std::optional<StreamAttributes> attr = streamIn->getAttributesBuffered()) //throw FileError
        attrSourceNew = *attr; //Native/MTP
    else //use more stale ones:
        attrSourceNew = attrSource; //SFTP/FTP
    //TODO: evaluate: consequences of stale attributes

    //target existing: undefined behavior! (fail/overwrite/auto-rename)
    auto streamOut = getOutputStream(apTarget, &attrSourceNew.fileSize, IOCallbackDivider(notifyUnbufferedIO, totalUnbufferedIO)); //throw FileError

    bufferedStreamCopy(*streamIn, *streamOut); //throw FileError, ErrorFileLocked, X

    const AFS::FileId targetFileId = streamOut->finalize(); //throw FileError, X

    //check if "expected == actual number of bytes written"
    //-> extra check: bytes reported via notifyUnbufferedIO() should match actual number of bytes written
    if (totalUnbufferedIO != 2 * makeSigned(attrSourceNew.fileSize))
        throw FileError(replaceCpy(_("Cannot read file %x."), L"%x", fmtPath(getDisplayPath(afsPathSource))),
                        replaceCpy(replaceCpy(_("Unexpected size of data stream.\nExpected: %x bytes\nActual: %y bytes"),
                                              L"%x", numberTo<std::wstring>(2 * attrSourceNew.fileSize)),
                                   L"%y", numberTo<std::wstring>(totalUnbufferedIO)) + L" [notifyUnbufferedIO]");

    std::optional<FileError> errorModTime;
    try
    {
        /*
        is setting modtime after closing the file handle a pessimization?
            Native: no, needed for functional correctness, see file_access.cpp
            MTP:    maybe a minor one (need to determine objectId one more time)
            SFTP:   no, needed for functional correctness (synology server), just as for Native
            FTP:    no: could set modtime via CURLOPT_POSTQUOTE (but this would internally trigger an extra round-trip anyway!)
        */
        setModTime(apTarget, attrSourceNew.modTime); //throw FileError, follows symlinks
    }
    catch (const FileError& e)
    {
        /*
        Failing to set modification time is not a serious problem from synchronization perspective (treated like external update)
          => Support additional scenarios:
            - GVFS failing to set modTime for FTP: https://freefilesync.org/forum/viewtopic.php?t=2372
            - GVFS failing to set modTime for MTP: https://freefilesync.org/forum/viewtopic.php?t=2803
            - MTP failing to set modTime in general: fail non-silently rather than silently during file creation
            - FTP failing to set modTime for servers lacking MFMT-support
        */
        errorModTime = FileError(e.toString()); //avoid slicing
    }

    AFS::FileCopyResult result;
    result.fileSize     = attrSourceNew.fileSize;
    result.modTime      = attrSourceNew.modTime;
    result.sourceFileId = attrSourceNew.fileId;
    result.targetFileId = targetFileId;
    result.errorModTime = errorModTime;
    return result;
}


//target existing: undefined behavior! (fail/overwrite/auto-rename)
AFS::FileCopyResult AFS::copyFileTransactional(const AbstractPath& apSource, const StreamAttributes& attrSource, //throw FileError, ErrorFileLocked
                                               const AbstractPath& apTarget,
                                               bool copyFilePermissions,
                                               bool transactionalCopy,
                                               const std::function<void()>& onDeleteTargetFile,
                                               const IOCallback& notifyUnbufferedIO)
{
    auto copyFilePlain = [&](const AbstractPath& apTargetTmp)
    {
        //caveat: typeid returns static type for pointers, dynamic type for references!!!
        if (typeid(*apSource.afs) == typeid(*apTargetTmp.afs))
            return apSource.afs->copyFileForSameAfsType(apSource.afsPath, attrSource,
                                                        apTargetTmp, copyFilePermissions, notifyUnbufferedIO); //throw FileError, ErrorFileLocked
        //target existing: undefined behavior! (fail/overwrite/auto-rename)

        //fall back to stream-based file copy:
        if (copyFilePermissions)
            throw FileError(replaceCpy(_("Cannot write permissions of %x."), L"%x", fmtPath(AFS::getDisplayPath(apTargetTmp))),
                            _("Operation not supported for different base folder types."));

        return apSource.afs->copyFileAsStream(apSource.afsPath, attrSource, apTargetTmp, notifyUnbufferedIO); //throw FileError, ErrorFileLocked
        //target existing: undefined behavior! (fail/overwrite/auto-rename)
    };

    if (transactionalCopy)
    {
        warn_static("doesnt make sense for Google Drive")


        std::optional<AbstractPath> parentPath = AFS::getParentFolderPath(apTarget);
        if (!parentPath)
            throw FileError(replaceCpy(_("Cannot write file %x."), L"%x", fmtPath(AFS::getDisplayPath(apTarget))), L"Path is device root.");
        const Zstring fileName = AFS::getItemName(apTarget);

        //- generate (hopefully) unique file name to avoid clashing with some remnant ffs_tmp file
        //- do not loop and avoid pathological cases, e.g. https://freefilesync.org/forum/viewtopic.php?t=1592
        const Zstring shortGuid = printNumber<Zstring>(Zstr("%04x"), static_cast<unsigned int>(getCrc16(generateGUID())));
        auto it = find_last(fileName.begin(), fileName.end(), Zstr('.')); //gracefully handle case of missing "."
        const Zstring fileNameTmp = Zstring(fileName.begin(), it) + Zstr('.') + shortGuid + TEMP_FILE_ENDING;

        const AbstractPath apTargetTmp = AFS::appendRelPath(*parentPath, fileNameTmp);
        //AbstractPath apTargetTmp(apTarget.afs, AfsPath(apTarget.afsPath.value + TEMP_FILE_ENDING));
        //-------------------------------------------------------------------------------------------

        const AFS::FileCopyResult result = copyFilePlain(apTargetTmp); //throw FileError, ErrorFileLocked

        //transactional behavior: ensure cleanup; not needed before copyFilePlain() which is already transactional
        ZEN_ON_SCOPE_FAIL( try { AFS::removeFilePlain(apTargetTmp); }
        catch (FileError&) {});

        //have target file deleted (after read access on source and target has been confirmed) => allow for almost transactional overwrite
        if (onDeleteTargetFile)
            onDeleteTargetFile(); //throw X

        //perf: this call is REALLY expensive on unbuffered volumes! ~40% performance decrease on FAT USB stick!
        moveAndRenameItem(apTargetTmp, apTarget); //throw FileError, (ErrorDifferentVolume)

        /*
            CAVEAT on FAT/FAT32: the sequence of deleting the target file and renaming "file.txt.ffs_tmp" to "file.txt" does
            NOT PRESERVE the creation time of the .ffs_tmp file, but SILENTLY "reuses" whatever creation time the old "file.txt" had!
            This "feature" is called "File System Tunneling":
            https://blogs.msdn.microsoft.com/oldnewthing/20050715-14/?p=34923
            http://support.microsoft.com/kb/172190/en-us
        */
        return result;
    }
    else
    {
        /*
           Note: non-transactional file copy solves at least four problems:
                -> skydrive - doesn't allow for .ffs_tmp extension and returns ERROR_INVALID_PARAMETER
                -> network renaming issues
                -> allow for true delete before copy to handle low disk space problems
                -> higher performance on non-buffered drives (e.g. usb sticks)
        */
        if (onDeleteTargetFile)
            onDeleteTargetFile();

        return copyFilePlain(apTarget); //throw FileError, ErrorFileLocked
    }
}


void AFS::createFolderIfMissingRecursion(const AbstractPath& ap) //throw FileError
{
    if (!getParentFolderPath(ap)) //device root
        return static_cast<void>(/*ItemType =*/ getItemType(ap)); //throw FileError

    try
    {
        //target existing: undefined behavior! (fail/overwrite)
        createFolderPlain(ap); //throw FileError
    }
    catch (FileError&)
    {
        const PathStatus ps = getPathStatus(ap); //throw FileError
        if (ps.existingType == ItemType::FILE)
            throw;

        //ps.relPath.size() == 1  => same createFolderPlain() call from above? Maybe parent folder was created by parallel thread shortly after failure!
        AbstractPath intermediatePath = ps.existingPath;
        for (const Zstring& itemName : ps.relPath)
            try
            {
                createFolderPlain(intermediatePath = appendRelPath(intermediatePath, itemName)); //throw FileError
            }
            catch (FileError&)
            {
                try //already existing => possible, if createFolderIfMissingRecursion() is run in parallel
                {
                    if (getItemType(intermediatePath) != ItemType::FILE) //throw FileError
                        continue;
                }
                catch (FileError&) {}

                throw;
            }
    }
}


//essentially a(n abstract) duplicate of zen::getPathStatus()
AFS::PathStatusImpl AFS::getPathStatusViaFolderTraversal(const AfsPath& afsPath) const //throw FileError
{
    const std::optional<AfsPath> parentAfsPath = getParentAfsPath(afsPath);
    try
    {
        return { getItemType(afsPath), afsPath, {} }; //throw FileError
    }
    catch (FileError&)
    {
        if (!parentAfsPath) //device root
            throw;
        //else: let's dig deeper... don't bother checking Win32 codes; e.g. not existing item may have the codes:
        //  ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_INVALID_NAME, ERROR_INVALID_DRIVE,
        //  ERROR_NOT_READY, ERROR_INVALID_PARAMETER, ERROR_BAD_PATHNAME, ERROR_BAD_NETPATH => not reliable
    }
    const Zstring itemName = getItemName(afsPath);
    assert(!itemName.empty());

    PathStatusImpl ps = getPathStatusViaFolderTraversal(*parentAfsPath); //throw FileError
    if (ps.relPath.empty() &&
        ps.existingType != ItemType::FILE) //obscure, but possible (and not an error)
        try
        {
            traverseFolderFlat(*parentAfsPath, //throw FileError
            [&](const FileInfo&    fi) { if (equalFilePath(fi.itemName, itemName)) throw ItemType::FILE;    },
            [&](const FolderInfo&  fi) { if (equalFilePath(fi.itemName, itemName)) throw ItemType::FOLDER;  },
            [&](const SymlinkInfo& si) { if (equalFilePath(si.itemName, itemName)) throw ItemType::SYMLINK; });
        }
        catch (const ItemType& type) { return { type, afsPath, {} }; } //yes, exceptions for control-flow are bad design... but, but...
    //we're not CPU-bound here and finding the item after getItemType() previously failed is exceptional (even C:\pagefile.sys should be found)

    ps.relPath.push_back(itemName);
    return ps;
}


std::optional<AFS::ItemType> AFS::getItemTypeIfExists(const AbstractPath& ap) //throw FileError
{
    const PathStatus ps = getPathStatus(ap); //throw FileError
    if (ps.relPath.empty())
        return ps.existingType;
    return {};
}


AFS::PathStatus AFS::getPathStatus(const AbstractPath& ap) //throw FileError
{
    const PathStatusImpl psi = ap.afs->getPathStatus(ap.afsPath); //throw FileError
    return { psi.existingType, AbstractPath(ap.afs, psi.existingAfsPath), psi.relPath };
}


void AFS::removeFolderIfExistsRecursion(const AbstractPath& ap, //throw FileError
                                        const std::function<void (const std::wstring& displayPath)>& onBeforeFileDeletion, //optional
                                        const std::function<void (const std::wstring& displayPath)>& onBeforeFolderDeletion) //one call for each object!
{
    std::function<void(const AbstractPath& folderPath)> removeFolderRecursionImpl;
    removeFolderRecursionImpl = [&onBeforeFileDeletion, &onBeforeFolderDeletion, &removeFolderRecursionImpl](const AbstractPath& folderPath) //throw FileError
    {
        //deferred recursion => save stack space and allow deletion of extremely deep hierarchies!
        std::vector<Zstring> fileNames;
        std::vector<Zstring> folderNames;
        std::vector<Zstring> symlinkNames;

        AFS::traverseFolderFlat(folderPath, //throw FileError
        [&](const AFS::FileInfo&    fi) { fileNames   .push_back(fi.itemName); },
        [&](const AFS::FolderInfo&  fi) { folderNames .push_back(fi.itemName); },
        [&](const AFS::SymlinkInfo& si) { symlinkNames.push_back(si.itemName); });

        for (const Zstring& fileName : fileNames)
        {
            const AbstractPath filePath = AFS::appendRelPath(folderPath, fileName);
            if (onBeforeFileDeletion)
                onBeforeFileDeletion(AFS::getDisplayPath(filePath));

            AFS::removeFilePlain(filePath); //throw FileError
        }

        for (const Zstring& symlinkName : symlinkNames)
        {
            const AbstractPath linkPath = AFS::appendRelPath(folderPath, symlinkName);
            if (onBeforeFileDeletion)
                onBeforeFileDeletion(AFS::getDisplayPath(linkPath)); //throw X

            AFS::removeSymlinkPlain(linkPath); //throw FileError
        }

        for (const Zstring& folderName : folderNames)
            removeFolderRecursionImpl(AFS::appendRelPath(folderPath, folderName)); //throw FileError

        if (onBeforeFolderDeletion)
            onBeforeFolderDeletion(AFS::getDisplayPath(folderPath)); //throw X

        AFS::removeFolderPlain(folderPath); //throw FileError
    };
    //--------------------------------------------------------------------------------------------------------------
    warn_static("what about parallelOps?")

    //no error situation if directory is not existing! manual deletion relies on it!
    if (std::optional<ItemType> type = AFS::getItemTypeIfExists(ap)) //throw FileError
    {
        if (*type == AFS::ItemType::SYMLINK)
        {
            if (onBeforeFileDeletion)
                onBeforeFileDeletion(AFS::getDisplayPath(ap));

            AFS::removeSymlinkPlain(ap); //throw FileError
        }
        else
            removeFolderRecursionImpl(ap); //throw FileError
    }
    else //even if the folder did not exist anymore, significant I/O work was done => report
        if (onBeforeFolderDeletion) onBeforeFolderDeletion(AFS::getDisplayPath(ap));
}


bool AFS::removeFileIfExists(const AbstractPath& ap) //throw FileError
{
    try
    {
        AFS::removeFilePlain(ap); //throw FileError
        return true;
    }
    catch (const FileError& e)
    {
        try
        {
            if (!AFS::getItemTypeIfExists(ap)) //throw FileError
                return false;
        }
        catch (const FileError& e2) { throw FileError(e.toString(), e2.toString()); } //unclear which exception is more relevant

        throw;
    }
}


bool AFS::removeSymlinkIfExists(const AbstractPath& ap) //throw FileError
{
    try
    {
        AFS::removeSymlinkPlain(ap); //throw FileError
        return true;
    }
    catch (const FileError& e)
    {
        try
        {
            if (!AFS::getItemTypeIfExists(ap)) //throw FileError
                return false;
        }
        catch (const FileError& e2) { throw FileError(e.toString(), e2.toString()); } //unclear which exception is more relevant

        throw;
    }
}


void AFS::removeEmptyFolderIfExists(const AbstractPath& ap) //throw FileError
{
    try
    {
        AFS::removeFolderPlain(ap); //throw FileError
    }
    catch (const FileError& e)
    {
        try
        {
            if (!AFS::getItemTypeIfExists(ap)) //throw FileError
                return;
        }
        catch (const FileError& e2) { throw FileError(e.toString(), e2.toString()); } //unclear which exception is more relevant

        throw;
    }
}

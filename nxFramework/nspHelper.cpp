#include "nspHelper.h"
#include "../tinfoil/install/simple_filesystem.hpp"
#include "../tinfoil/nx/content_meta.hpp"
#include "../tinfoil/nx/ncm.hpp"
#include "../tinfoil/util/title_util.hpp"
#include <iostream>
#include <memory>

using namespace tin::install::nsp;


  std::tuple<std::string, nx::ncm::ContentRecord> GetCNMTNCAInfo(std::string nspPath)
    {
        // Open filesystem
        nx::fs::IFileSystem fileSystem;
        std::string nspExt          = ".nsp";
        std::string rootPath        = "/";
        std::string absolutePath    = nspPath + "/";

        // Check if this is an nsp file
        if (nspPath.compare(nspPath.size() - nspExt.size(), nspExt.size(), nspExt) == 0)
        {
            fileSystem.OpenFileSystemWithId(nspPath, FsFileSystemType_ApplicationPackage, 0);
        }
        else // Otherwise assume this is an extracted NSP directory
        {
            fileSystem.OpenSdFileSystem();
            rootPath        = nspPath.substr(9) + "/";
            absolutePath    = nspPath + "/";
        }
        tin::install::nsp::SimpleFileSystem simpleFS(fileSystem, rootPath, absolutePath);

        // Create the path of the cnmt NCA
        auto cnmtNCAName        = simpleFS.GetFileNameFromExtension("", "cnmt.nca");
        auto cnmtNCAFile        = simpleFS.OpenFile(cnmtNCAName);
        auto cnmtNCAFullPath    = simpleFS.m_absoluteRootPath + cnmtNCAName;
        u64 cnmtNCASize         = cnmtNCAFile.GetSize();

        // Prepare cnmt content record
        nx::ncm::ContentRecord contentRecord;
        contentRecord.ncaId         = tin::util::GetNcaIdFromString(cnmtNCAName);
        *(u64*)contentRecord.size   = cnmtNCASize & 0xFFFFFFFFFFFF;
        contentRecord.contentType   = nx::ncm::ContentType::META;
        return { cnmtNCAFullPath, contentRecord };
    }

    nx::ncm::ContentMeta GetContentMetaFromNCA(std::string& ncaPath)
    {
        // Create the cnmt filesystem
        nx::fs::IFileSystem cnmtNCAFileSystem;
        cnmtNCAFileSystem.OpenFileSystemWithId(ncaPath, FsFileSystemType_ContentMeta, 0);
        tin::install::nsp::SimpleFileSystem cnmtNCASimpleFileSystem(cnmtNCAFileSystem, "/", ncaPath + "/");

        // Find and read the cnmt file
        auto cnmtName = cnmtNCASimpleFileSystem.GetFileNameFromExtension("", "cnmt");
        auto cnmtFile = cnmtNCASimpleFileSystem.OpenFile(cnmtName);
        u64 cnmtSize  = cnmtFile.GetSize();

        tin::util::ByteBuffer cnmtBuf;
        cnmtBuf.Resize(cnmtSize);
        cnmtFile.Read(0x0, cnmtBuf.GetData(), cnmtSize);

        return nx::ncm::ContentMeta(cnmtBuf.GetData(), cnmtBuf.GetSize());
    }

std::tuple<nx::ncm::ContentMeta, nx::ncm::ContentRecord> ReadCNMT(SimpleFileSystem& simpleFS)
{
    std::tuple<std::string, nx::ncm::ContentRecord> cnmtNCAInfo = GetCNMTNCAInfo(simpleFS.m_absoluteRootPath.substr(0, simpleFS.m_absoluteRootPath.size() - 1));
    return { GetContentMetaFromNCA(std::get<0>(cnmtNCAInfo)), std::get<1>(cnmtNCAInfo) };
}

void InstallTicketCert(SimpleFileSystem& simpleFS)
{
    // Read the tik file and put it into a buffer
    auto tikName    = simpleFS.GetFileNameFromExtension("", "tik");     LOG("> Getting tik size\n");
    auto tikFile    = simpleFS.OpenFile(tikName);
    u64 tikSize     = tikFile.GetSize();
    auto tikBuf     = std::make_unique<u8[]>(tikSize);                  LOG("> Reading tik\n");
    tikFile.Read(0x0, tikBuf.get(), tikSize);

    // Read the cert file and put it into a buffer
    auto certName   = simpleFS.GetFileNameFromExtension("", "cert");    LOG("> Getting cert size\n");
    auto certFile   = simpleFS.OpenFile(certName);
    u64 certSize    = certFile.GetSize();
    auto certBuf    = std::make_unique<u8[]>(certSize);                 LOG("> Reading cert\n");
    certFile.Read(0x0, certBuf.get(), certSize);

    // Finally, let's actually import the ticket
    if(R_FAILED(esImportTicket(tikBuf.get(), tikSize, certBuf.get(), certSize)))
        LOG("Failed to import ticket\n");
}

void InstallContentMetaRecords(tin::util::ByteBuffer&     installContentMetaBuf,
                               nx::ncm::ContentMeta&      contentMeta,
                               const FsStorageId          destStorageId)
{
    NcmContentMetaDatabase contentMetaDatabase;
    NcmMetaRecord contentMetaKey = contentMeta.GetContentMetaKey();
    try
    {
        if(R_FAILED(ncmOpenContentMetaDatabase(destStorageId, &contentMetaDatabase)))
            LOG("Failed to open content meta database\n");
        if(R_FAILED(ncmContentMetaDatabaseSet(&contentMetaDatabase, &contentMetaKey, installContentMetaBuf.GetSize(), (NcmContentMetaRecordsHeader*)installContentMetaBuf.GetData())))
            LOG("Failed to set content records\n");
        if(R_FAILED(ncmContentMetaDatabaseCommit(&contentMetaDatabase)))
            LOG("Failed to commit content records\n");
    }
    catch (std::runtime_error& e)
    {
        serviceClose(&contentMetaDatabase.s);
        throw e;
    }
}

void InstallApplicationRecord(nx::ncm::ContentMeta& contentMeta, const FsStorageId destStorageId)
{
    Result rc = 0;
    std::vector<ContentStorageRecord> storageRecords;
    u64 baseTitleId = tin::util::GetBaseTitleId(
            contentMeta.GetContentMetaKey().titleId,
            static_cast<nx::ncm::ContentMetaType>(contentMeta.GetContentMetaKey().type));

    LOG("Base title Id: 0x%lx\n", baseTitleId);

    // TODO: Make custom error with result code field
    // 0x410: The record doesn't already exist
    u32 contentMetaCount = 0;
    if (R_FAILED(rc = nsCountApplicationContentMeta(baseTitleId, &contentMetaCount)) && rc != 0x410)
    {
        throw std::runtime_error("Failed to count application content meta\n");
    }
    LOG("Content meta count: %u\n", contentMetaCount);

    // Obtain any existing app record content meta and append it to our vector
    if (contentMetaCount > 0)
    {
        storageRecords.resize(contentMetaCount);
        size_t contentStorageBufSize    = contentMetaCount * sizeof(ContentStorageRecord);
        auto contentStorageBuf          = std::make_unique<ContentStorageRecord[]>(contentMetaCount);
        u32 entriesRead;

        if (R_FAILED(nsListApplicationRecordContentMeta(0, baseTitleId, contentStorageBuf.get(), contentStorageBufSize, &entriesRead)))
            LOG("Failed to list application record content meta\n");

        if (entriesRead != contentMetaCount)
        {
            throw std::runtime_error("Mismatch between entries read and content meta count\n");
        }
        memcpy(storageRecords.data(), contentStorageBuf.get(), contentStorageBufSize);
    }

    // Add our new content meta
    ContentStorageRecord storageRecord;
    storageRecord.metaRecord = contentMeta.GetContentMetaKey();
    storageRecord.storageId  = destStorageId;
    storageRecords.push_back(storageRecord);

    // Replace the existing application records with our own
    try
    {
        nsDeleteApplicationRecord(baseTitleId);
    }
    catch (...) {}
    LOG("Pushing application record...\n");
    if (R_FAILED(nsPushApplicationRecord(baseTitleId, 0x3, storageRecords.data(), storageRecords.size() * sizeof(ContentStorageRecord))))
        LOG("Failed to push application record\n");
}

void InstallNCA(SimpleFileSystem& simpleFS, const NcmNcaId& ncaId, const FsStorageId destStorageId)
{
    std::string ncaName = tin::util::GetNcaIdString(ncaId);

    if (simpleFS.HasFile(ncaName + ".nca"))
        ncaName += ".nca";
    else
    if (simpleFS.HasFile(ncaName + ".cnmt.nca"))
        ncaName += ".cnmt.nca";
    else
    {
        throw std::runtime_error(("Failed to find NCA file " + ncaName + ".nca/.cnmt.nca").c_str());
    }

    LOG("NcaId: %s\n", ncaName.c_str());
    LOG("Dest storage Id: %u\n", destStorageId);

    nx::ncm::ContentStorage contentStorage(destStorageId);

    // Attempt to delete any leftover placeholders
    try
    {
        contentStorage.DeletePlaceholder(ncaId);
    }
    catch (...) {}

    auto ncaFile    = simpleFS.OpenFile(ncaName);
    size_t ncaSize  = ncaFile.GetSize();
    u64 fileOff     = 0;
    size_t readSize = 0x400000; // 4MB buff
    auto readBuffer = std::make_unique<u8[]>(readSize);

    if (readBuffer == NULL)
        throw std::runtime_error(("Failed to allocate read buffer for " + ncaName).c_str());

    LOG("Size: 0x%lx\n", ncaSize);
    contentStorage.CreatePlaceholder(ncaId, ncaId, ncaSize);

    float progress;

    while (fileOff < ncaSize)
    {
        // Clear the buffer before we read anything, just to be sure
        progress = (float)fileOff / (float)ncaSize;

        if (fileOff % (0x400000 * 3) == 0)
            LOG("> Progress: %lu/%lu MB (%d%s)\n", (fileOff / 1000000), (ncaSize / 1000000), (int)(progress * 100.0), "%");

        if (fileOff + readSize >= ncaSize)
            readSize = ncaSize - fileOff;

        ncaFile.Read(fileOff, readBuffer.get(), readSize);
        contentStorage.WritePlaceholder(ncaId, fileOff, readBuffer.get(), readSize);
        fileOff += readSize;
    }

    // Clean up the line for whatever comes next
    LOG("                                                           \n");
    LOG("Registering placeholder...\n");

    try
    {
        contentStorage.Register(ncaId, ncaId);
    }
    catch (...)
    {
        LOG(("Failed to register " + ncaName + ". It may already exist.\n").c_str());
    }
    try
    {
        contentStorage.DeletePlaceholder(ncaId);
    }
    catch (...) {}
}

bool InstallNSP(const std::string& filename, const FsStorageId destStorageId, const bool ignoreReqFirmVersion)
{
    try
    {
        nx::fs::IFileSystem fileSystem;
        fileSystem.OpenFileSystemWithId(filename, FsFileSystemType_ApplicationPackage, 0);
        SimpleFileSystem simpleFS(fileSystem, "/", filename + "/");

        /////////////////// Prepare
        LOG("Preparing install...\n");

        tin::util::ByteBuffer cnmtBuf;
        auto cnmtTuple = ReadCNMT(simpleFS);
        nx::ncm::ContentMeta    contentMeta       = std::get<0>(cnmtTuple);
        nx::ncm::ContentRecord  cnmtContentRecord = std::get<1>(cnmtTuple);
        nx::ncm::ContentStorage contentStorage(destStorageId);
        if (!contentStorage.Has(cnmtContentRecord.ncaId))
        {
            LOG("Installing CNMT NCA...\n");
            InstallNCA(simpleFS, cnmtContentRecord.ncaId, destStorageId);
        }
        else
        {
            LOG("CNMT NCA already installed. Proceeding...\n");
        }

        // Parse data and create install content meta
        if (ignoreReqFirmVersion)
            LOG("WARNING: Required system firmware version is being IGNORED!\n");

        tin::util::ByteBuffer installContentMetaBuf;
        contentMeta.GetInstallContentMeta(installContentMetaBuf, cnmtContentRecord, ignoreReqFirmVersion);

        InstallContentMetaRecords(installContentMetaBuf, contentMeta, destStorageId);
        InstallApplicationRecord(contentMeta, destStorageId);

        LOG("Installing ticket and cert...\n");
        try
        {
            InstallTicketCert(simpleFS);
        }
        catch (std::runtime_error& e)
        {
            LOG("WARNING: Ticket installation failed! This may not be an issue, depending on your usecase.\nProceed with caution!\n");
            return false;
        }
        ///////////////////


        LOG("Pre Install Records: \n");
        //task.DebugPrintInstallData();


        /////////////////// Begin
        LOG("Installing NCAs...\n");
        for (auto& record : contentMeta.GetContentRecords())
        {
            LOG("Installing from %s\n", tin::util::GetNcaIdString(record.ncaId).c_str());
            InstallNCA(simpleFS, record.ncaId, destStorageId);
        }
        LOG("Post Install Records: \n");
        //this->DebugPrintInstallData();
        ///////////////////


        LOG("Post Install Records: \n");
        //task.DebugPrintInstallData();
    }
    catch (std::exception& e)
    {
        LOG("Failed to install NSP!\n");
        LOG("%s", e.what());
        return false;
    }
    return true;
}

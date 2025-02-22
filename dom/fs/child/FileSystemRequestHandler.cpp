/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "fs/FileSystemRequestHandler.h"

#include "FileSystemEntryMetadataArray.h"
#include "fs/FileSystemConstants.h"
#include "mozilla/dom/File.h"
#include "mozilla/dom/FileSystemDirectoryHandle.h"
#include "mozilla/dom/FileSystemFileHandle.h"
#include "mozilla/dom/FileSystemHandle.h"
#include "mozilla/dom/FileSystemManager.h"
#include "mozilla/dom/FileSystemManagerChild.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/quota/QuotaCommon.h"

namespace mozilla::dom::fs {

using mozilla::ipc::RejectCallback;

namespace {

// TODO: This is just a dummy implementation
RefPtr<File> MakeGetFileResult(nsIGlobalObject* aGlobal, const nsString& aName,
                               const nsString& aType,
                               int64_t aLastModifiedMilliSeconds,
                               nsTArray<Name>&& aPath, IPCBlob&& /* aFile */,
                               RefPtr<FileSystemManager>& aManager) {
  // TODO: Replace with a real implementation
  RefPtr<File> result = File::CreateMemoryFileWithCustomLastModified(
      aGlobal, static_cast<void*>(new uint8_t[1]), sizeof(uint8_t), aName,
      aType, aLastModifiedMilliSeconds);

  return result;
}

bool MakeResolution(nsIGlobalObject* aGlobal,
                    FileSystemGetEntriesResponse&& aResponse,
                    const bool& /* aResult */,
                    RefPtr<FileSystemEntryMetadataArray>& aSink) {
  // TODO: Add page size to FileSystemConstants, preallocate and handle overflow
  const auto& listing = aResponse.get_FileSystemDirectoryListing();

  for (const auto& it : listing.files()) {
    aSink->AppendElement(it);
  }

  for (const auto& it : listing.directories()) {
    aSink->AppendElement(it);
  }

  return true;
}

RefPtr<FileSystemDirectoryHandle> MakeResolution(
    nsIGlobalObject* aGlobal, FileSystemGetHandleResponse&& aResponse,
    const RefPtr<FileSystemDirectoryHandle>& /* aResult */,
    RefPtr<FileSystemManager>& aManager) {
  RefPtr<FileSystemDirectoryHandle> result = new FileSystemDirectoryHandle(
      aGlobal, aManager,
      FileSystemEntryMetadata(aResponse.get_EntryId(), kRootName,
                              /* directory */ true));
  return result;
}

RefPtr<FileSystemDirectoryHandle> MakeResolution(
    nsIGlobalObject* aGlobal, FileSystemGetHandleResponse&& aResponse,
    const RefPtr<FileSystemDirectoryHandle>& /* aResult */, const Name& aName,
    RefPtr<FileSystemManager>& aManager) {
  RefPtr<FileSystemDirectoryHandle> result = new FileSystemDirectoryHandle(
      aGlobal, aManager,
      FileSystemEntryMetadata(aResponse.get_EntryId(), aName,
                              /* directory */ true));

  return result;
}

RefPtr<FileSystemFileHandle> MakeResolution(
    nsIGlobalObject* aGlobal, FileSystemGetHandleResponse&& aResponse,
    const RefPtr<FileSystemFileHandle>& /* aResult */, const Name& aName,
    RefPtr<FileSystemManager>& aManager) {
  RefPtr<FileSystemFileHandle> result = new FileSystemFileHandle(
      aGlobal, aManager,
      FileSystemEntryMetadata(aResponse.get_EntryId(), aName,
                              /* directory */ false));
  return result;
}

RefPtr<File> MakeResolution(nsIGlobalObject* aGlobal,
                            FileSystemGetFileResponse&& aResponse,
                            const RefPtr<File>& /* aResult */,
                            const Name& aName,
                            RefPtr<FileSystemManager>& aManager) {
  auto& fileProperties = aResponse.get_FileSystemFileProperties();
  return MakeGetFileResult(aGlobal, aName, fileProperties.type(),
                           fileProperties.last_modified_ms(),
                           std::move(fileProperties.path()),
                           std::move(fileProperties.file()), aManager);
}

template <class TResponse, class... Args>
void ResolveCallback(
    TResponse&& aResponse,
    RefPtr<Promise> aPromise,  // NOLINT(performance-unnecessary-value-param)
    Args&&... args) {
  MOZ_ASSERT(aPromise);
  QM_TRY(OkIf(Promise::PromiseState::Pending == aPromise->State()), QM_VOID);

  if (TResponse::Tnsresult == aResponse.type()) {
    aPromise->MaybeReject(aResponse.get_nsresult());
    return;
  }

  aPromise->MaybeResolve(MakeResolution(aPromise->GetParentObject(),
                                        std::forward<TResponse>(aResponse),
                                        std::forward<Args>(args)...));
}

template <>
void ResolveCallback(
    FileSystemRemoveEntryResponse&& aResponse,
    RefPtr<Promise> aPromise) {  // NOLINT(performance-unnecessary-value-param)
  MOZ_ASSERT(aPromise);
  QM_TRY(OkIf(Promise::PromiseState::Pending == aPromise->State()), QM_VOID);

  if (FileSystemRemoveEntryResponse::Tvoid_t == aResponse.type()) {
    aPromise->MaybeResolveWithUndefined();
    return;
  }

  MOZ_ASSERT(FileSystemRemoveEntryResponse::Tnsresult == aResponse.type());
  const auto& status = aResponse.get_nsresult();
  if (NS_ERROR_FILE_ACCESS_DENIED == status) {
    aPromise->MaybeRejectWithNotAllowedError("Permission denied");
  } else if (NS_ERROR_DOM_FILESYSTEM_NO_MODIFICATION_ALLOWED_ERR == status) {
    aPromise->MaybeRejectWithInvalidModificationError("Disallowed by system");
  } else if (NS_FAILED(status)) {
    aPromise->MaybeRejectWithUnknownError("Unknown failure");
  } else {
    aPromise->MaybeResolveWithUndefined();
  }
}

template <>
void ResolveCallback(FileSystemResolveResponse&& aResponse,
                     // NOLINTNEXTLINE(performance-unnecessary-value-param)
                     RefPtr<Promise> aPromise) {
  MOZ_ASSERT(aPromise);
  QM_TRY(OkIf(Promise::PromiseState::Pending == aPromise->State()), QM_VOID);

  if (FileSystemResolveResponse::Tnsresult == aResponse.type()) {
    aPromise->MaybeReject(aResponse.get_nsresult());
    return;
  }

  auto& maybePath = aResponse.get_MaybeFileSystemPath();
  if (maybePath.isSome()) {
    aPromise->MaybeResolve(maybePath.value().path());
    return;
  }

  aPromise->MaybeResolveWithUndefined();
}

template <class TResponse, class TReturns, class... Args,
          std::enable_if_t<std::is_same<TReturns, void>::value, bool> = true>
mozilla::ipc::ResolveCallback<TResponse> SelectResolveCallback(
    RefPtr<Promise> aPromise,  // NOLINT(performance-unnecessary-value-param)
    Args&&... args) {
  using TOverload = void (*)(TResponse&&, RefPtr<Promise>, Args...);
  return static_cast<std::function<void(TResponse &&)>>(
      // NOLINTNEXTLINE(modernize-avoid-bind)
      std::bind(static_cast<TOverload>(ResolveCallback), std::placeholders::_1,
                aPromise, std::forward<Args>(args)...));
}

template <class TResponse, class TReturns, class... Args,
          std::enable_if_t<!std::is_same<TReturns, void>::value, bool> = true>
mozilla::ipc::ResolveCallback<TResponse> SelectResolveCallback(
    RefPtr<Promise> aPromise,  // NOLINT(performance-unnecessary-value-param)
    Args&&... args) {
  using TOverload =
      void (*)(TResponse&&, RefPtr<Promise>, const TReturns&, Args...);
  return static_cast<std::function<void(TResponse &&)>>(
      // NOLINTNEXTLINE(modernize-avoid-bind)
      std::bind(static_cast<TOverload>(ResolveCallback), std::placeholders::_1,
                aPromise, TReturns(), std::forward<Args>(args)...));
}

// TODO: Find a better way to deal with these errors
void IPCRejectReporter(mozilla::ipc::ResponseRejectReason aReason) {
  switch (aReason) {
    case mozilla::ipc::ResponseRejectReason::ActorDestroyed:
      // This is ok
      break;
    case mozilla::ipc::ResponseRejectReason::HandlerRejected:
      QM_TRY(OkIf(false), QM_VOID);
      break;
    case mozilla::ipc::ResponseRejectReason::ChannelClosed:
      QM_TRY(OkIf(false), QM_VOID);
      break;
    case mozilla::ipc::ResponseRejectReason::ResolverDestroyed:
      QM_TRY(OkIf(false), QM_VOID);
      break;
    case mozilla::ipc::ResponseRejectReason::SendError:
      QM_TRY(OkIf(false), QM_VOID);
      break;
    default:
      QM_TRY(OkIf(false), QM_VOID);
      break;
  }
}

void RejectCallback(
    RefPtr<Promise> aPromise,  // NOLINT(performance-unnecessary-value-param)
    mozilla::ipc::ResponseRejectReason aReason) {
  IPCRejectReporter(aReason);
  QM_TRY(OkIf(Promise::PromiseState::Pending == aPromise->State()), QM_VOID);
  aPromise->MaybeRejectWithUndefined();
}

mozilla::ipc::RejectCallback GetRejectCallback(
    RefPtr<Promise> aPromise) {  // NOLINT(performance-unnecessary-value-param)
  return static_cast<mozilla::ipc::RejectCallback>(
      // NOLINTNEXTLINE(modernize-avoid-bind)
      std::bind(RejectCallback, aPromise, std::placeholders::_1));
}

}  // namespace

void FileSystemRequestHandler::GetRootHandle(
    RefPtr<FileSystemManager>
        aManager,                // NOLINT(performance-unnecessary-value-param)
    RefPtr<Promise> aPromise) {  // NOLINT(performance-unnecessary-value-param)
  MOZ_ASSERT(aManager);
  MOZ_ASSERT(aPromise);

  auto&& onResolve = SelectResolveCallback<FileSystemGetHandleResponse,
                                           RefPtr<FileSystemDirectoryHandle>>(
      aPromise, aManager);

  auto&& onReject = GetRejectCallback(aPromise);

  QM_TRY(OkIf(aManager->Actor()), QM_VOID, [aPromise](const auto&) {
    aPromise->MaybeRejectWithUnknownError("Invalid actor");
  });
  aManager->Actor()->SendGetRootHandle(std::move(onResolve),
                                       std::move(onReject));
}

void FileSystemRequestHandler::GetDirectoryHandle(
    RefPtr<FileSystemManager>& aManager,
    const FileSystemChildMetadata& aDirectory, bool aCreate,
    RefPtr<Promise> aPromise) {  // NOLINT(performance-unnecessary-value-param)
  MOZ_ASSERT(aManager);
  MOZ_ASSERT(!aDirectory.parentId().IsEmpty());
  MOZ_ASSERT(aPromise);

  FileSystemGetHandleRequest request(aDirectory, aCreate);

  auto&& onResolve = SelectResolveCallback<FileSystemGetHandleResponse,
                                           RefPtr<FileSystemDirectoryHandle>>(
      aPromise, aDirectory.childName(), aManager);

  auto&& onReject = GetRejectCallback(aPromise);

  QM_TRY(OkIf(aManager->Actor()), QM_VOID, [aPromise](const auto&) {
    aPromise->MaybeRejectWithUnknownError("Invalid actor");
  });
  aManager->Actor()->SendGetDirectoryHandle(request, std::move(onResolve),
                                            std::move(onReject));
}

void FileSystemRequestHandler::GetFileHandle(
    RefPtr<FileSystemManager>& aManager, const FileSystemChildMetadata& aFile,
    bool aCreate,
    RefPtr<Promise> aPromise) {  // NOLINT(performance-unnecessary-value-param)
  MOZ_ASSERT(aManager);
  MOZ_ASSERT(!aFile.parentId().IsEmpty());
  MOZ_ASSERT(aPromise);

  FileSystemGetHandleRequest request(aFile, aCreate);

  auto&& onResolve = SelectResolveCallback<FileSystemGetHandleResponse,
                                           RefPtr<FileSystemFileHandle>>(
      aPromise, aFile.childName(), aManager);

  auto&& onReject = GetRejectCallback(aPromise);

  QM_TRY(OkIf(aManager->Actor()), QM_VOID, [aPromise](const auto&) {
    aPromise->MaybeRejectWithUnknownError("Invalid actor");
  });
  aManager->Actor()->SendGetFileHandle(request, std::move(onResolve),
                                       std::move(onReject));
}

void FileSystemRequestHandler::GetFile(
    RefPtr<FileSystemManager>& aManager, const FileSystemEntryMetadata& aFile,
    RefPtr<Promise> aPromise) {  // NOLINT(performance-unnecessary-value-param)
  MOZ_ASSERT(aManager);
  MOZ_ASSERT(!aFile.entryId().IsEmpty());
  MOZ_ASSERT(aPromise);

  FileSystemGetFileRequest request(aFile.entryId());

  auto&& onResolve =
      SelectResolveCallback<FileSystemGetFileResponse, RefPtr<File>>(
          aPromise, aFile.entryName(), aManager);

  auto&& onReject = GetRejectCallback(aPromise);

  QM_TRY(OkIf(aManager->Actor()), QM_VOID, [aPromise](const auto&) {
    aPromise->MaybeRejectWithUnknownError("Invalid actor");
  });
  aManager->Actor()->SendGetFile(request, std::move(onResolve),
                                 std::move(onReject));
}

void FileSystemRequestHandler::GetEntries(
    RefPtr<FileSystemManager>& aManager, const EntryId& aDirectory,
    PageNumber aPage,
    RefPtr<Promise> aPromise,  // NOLINT(performance-unnecessary-value-param)
    RefPtr<FileSystemEntryMetadataArray>& aSink) {
  MOZ_ASSERT(aManager);
  MOZ_ASSERT(!aDirectory.IsEmpty());
  MOZ_ASSERT(aPromise);

  FileSystemGetEntriesRequest request(aDirectory, aPage);

  auto&& onResolve = SelectResolveCallback<FileSystemGetEntriesResponse, bool>(
      aPromise, aSink);

  auto&& onReject = GetRejectCallback(aPromise);

  QM_TRY(OkIf(aManager->Actor()), QM_VOID, [aPromise](const auto&) {
    aPromise->MaybeRejectWithUnknownError("Invalid actor");
  });
  aManager->Actor()->SendGetEntries(request, std::move(onResolve),
                                    std::move(onReject));
}

void FileSystemRequestHandler::RemoveEntry(
    RefPtr<FileSystemManager>& aManager, const FileSystemChildMetadata& aEntry,
    bool aRecursive,
    RefPtr<Promise> aPromise) {  // NOLINT(performance-unnecessary-value-param)
  MOZ_ASSERT(aManager);
  MOZ_ASSERT(!aEntry.parentId().IsEmpty());
  MOZ_ASSERT(aPromise);

  FileSystemRemoveEntryRequest request(aEntry, aRecursive);

  auto&& onResolve =
      SelectResolveCallback<FileSystemRemoveEntryResponse, void>(aPromise);

  auto&& onReject = GetRejectCallback(aPromise);

  QM_TRY(OkIf(aManager->Actor()), QM_VOID, [aPromise](const auto&) {
    aPromise->MaybeRejectWithUnknownError("Invalid actor");
  });
  aManager->Actor()->SendRemoveEntry(request, std::move(onResolve),
                                     std::move(onReject));
}

void FileSystemRequestHandler::Resolve(
    RefPtr<FileSystemManager>& aManager,
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    const FileSystemEntryPair& aEndpoints, RefPtr<Promise> aPromise) {
  MOZ_ASSERT(aManager);
  MOZ_ASSERT(!aEndpoints.parentId().IsEmpty());
  MOZ_ASSERT(!aEndpoints.childId().IsEmpty());
  MOZ_ASSERT(aPromise);

  FileSystemResolveRequest request(aEndpoints);

  auto&& onResolve =
      SelectResolveCallback<FileSystemResolveResponse, void>(aPromise);

  auto&& onReject = GetRejectCallback(aPromise);

  QM_TRY(OkIf(aManager->Actor()), QM_VOID, [aPromise](const auto&) {
    aPromise->MaybeRejectWithUnknownError("Invalid actor");
  });
  aManager->Actor()->SendResolve(request, std::move(onResolve),
                                 std::move(onReject));
}

}  // namespace mozilla::dom::fs

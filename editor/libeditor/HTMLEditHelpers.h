/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_HTMLEditHelpers_h
#define mozilla_HTMLEditHelpers_h

/**
 * This header declares/defines trivial helper classes which are used by
 * HTMLEditor.  If you want to create or look for static utility methods,
 * see HTMLEditUtils.h.
 */

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/Attributes.h"
#include "mozilla/ContentIterator.h"
#include "mozilla/EditorDOMPoint.h"
#include "mozilla/EditorForwards.h"
#include "mozilla/EditorUtils.h"  // for SuggestCaretOption(s)
#include "mozilla/IntegerRange.h"
#include "mozilla/Maybe.h"
#include "mozilla/RangeBoundary.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/StaticRange.h"
#include "nsCOMPtr.h"
#include "nsDebug.h"
#include "nsError.h"
#include "nsIContent.h"
#include "nsRange.h"
#include "nsString.h"

class nsISimpleEnumerator;

namespace mozilla {

// JoinNodesDirection is also affected to which one is new node at splitting
// a node because a couple of undo/redo.
enum class JoinNodesDirection {
  LeftNodeIntoRightNode,
  RightNodeIntoLeftNode,
};

static inline std::ostream& operator<<(std::ostream& aStream,
                                       JoinNodesDirection aJoinNodesDirection) {
  if (aJoinNodesDirection == JoinNodesDirection::LeftNodeIntoRightNode) {
    return aStream << "JoinNodesDirection::LeftNodeIntoRightNode";
  }
  if (aJoinNodesDirection == JoinNodesDirection::RightNodeIntoLeftNode) {
    return aStream << "JoinNodesDirection::RightNodeIntoLeftNode";
  }
  return aStream << "Invalid value";
}

// SplitNodeDirection is also affected to which one is removed at joining a
// node because a couple of undo/redo.
enum class SplitNodeDirection {
  LeftNodeIsNewOne,
  RightNodeIsNewOne,
};

static inline std::ostream& operator<<(std::ostream& aStream,
                                       SplitNodeDirection aSplitNodeDirection) {
  if (aSplitNodeDirection == SplitNodeDirection::LeftNodeIsNewOne) {
    return aStream << "SplitNodeDirection::LeftNodeIsNewOne";
  }
  if (aSplitNodeDirection == SplitNodeDirection::RightNodeIsNewOne) {
    return aStream << "SplitNodeDirection::RightNodeIsNewOne";
  }
  return aStream << "Invalid value";
}

/*****************************************************************************
 * MoveNodeResult is a simple class for MoveSomething() methods.
 * This holds error code and next insertion point if moving contents succeeded.
 * TODO: Perhaps, we can make this inherits mozilla::Result for guaranteeing
 *       same API.  Then, changing to/from Result<*, nsresult> can be easier.
 *       For now, we should give same API name rather than same as
 *       mozilla::ErrorResult.
 *****************************************************************************/
class MOZ_STACK_CLASS MoveNodeResult final {
 public:
  // FYI: NS_SUCCEEDED and NS_FAILED contain MOZ_(UN)LIKELY so that isOk() and
  //      isErr() must not required to wrap with them.
  bool isOk() const { return NS_SUCCEEDED(mRv); }
  bool isErr() const { return NS_FAILED(mRv); }
  constexpr bool Handled() const { return mHandled; }
  constexpr bool Ignored() const { return !Handled(); }
  constexpr nsresult inspectErr() const { return mRv; }
  constexpr nsresult unwrapErr() const { return mRv; }
  constexpr bool EditorDestroyed() const {
    return MOZ_UNLIKELY(mRv == NS_ERROR_EDITOR_DESTROYED);
  }
  constexpr bool NotInitialized() const {
    return mRv == NS_ERROR_NOT_INITIALIZED;
  }
  constexpr const EditorDOMPoint& NextInsertionPointRef() const {
    return mNextInsertionPoint;
  }
  EditorDOMPoint NextInsertionPoint() const { return mNextInsertionPoint; }

  void MarkAsHandled() { mHandled = true; }

  /**
   * Suggest caret position to aHTMLEditor.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult SuggestCaretPointTo(
      const HTMLEditor& aHTMLEditor, const SuggestCaretOptions& aOptions) const;

  /**
   * IgnoreCaretPointSuggestion() should be called if the method does not want
   * to use caret position recommended by this instance.
   */
  void IgnoreCaretPointSuggestion() const { mHandledCaretPoint = true; }

  bool HasCaretPointSuggestion() const { return mCaretPoint.IsSet(); }
  constexpr EditorDOMPoint&& UnwrapCaretPoint() {
    mHandledCaretPoint = true;
    return std::move(mCaretPoint);
  }
  bool MoveCaretPointTo(EditorDOMPoint& aPointToPutCaret,
                        const SuggestCaretOptions& aOptions) {
    MOZ_ASSERT(!aOptions.contains(SuggestCaret::AndIgnoreTrivialError));
    MOZ_ASSERT(
        !aOptions.contains(SuggestCaret::OnlyIfTransactionsAllowedToDoIt));
    if (aOptions.contains(SuggestCaret::OnlyIfHasSuggestion) &&
        !mCaretPoint.IsSet()) {
      return false;
    }
    aPointToPutCaret = UnwrapCaretPoint();
    return true;
  }
  bool MoveCaretPointTo(EditorDOMPoint& aPointToPutCaret,
                        const HTMLEditor& aHTMLEditor,
                        const SuggestCaretOptions& aOptions);

  MoveNodeResult() : mRv(NS_ERROR_NOT_INITIALIZED), mHandled(false) {}

  explicit MoveNodeResult(nsresult aRv) : mRv(aRv), mHandled(false) {
    MOZ_DIAGNOSTIC_ASSERT(NS_FAILED(mRv));
  }

  MoveNodeResult(const MoveNodeResult& aOther) = delete;
  MoveNodeResult& operator=(const MoveNodeResult& aOther) = delete;
  MoveNodeResult(MoveNodeResult&& aOther) = default;
  MoveNodeResult& operator=(MoveNodeResult&& aOther) = default;

  MoveNodeResult& operator|=(const MoveNodeResult& aOther) {
    // aOther is merged with this instance so that its caret suggestion
    // shouldn't be handled anymore.
    aOther.mHandledCaretPoint = true;
    // Should be handled again even if it's already handled
    mHandledCaretPoint = false;

    mHandled |= aOther.mHandled;
    // When both result are same, keep the result but use newer point.
    if (mRv == aOther.mRv) {
      mNextInsertionPoint = aOther.mNextInsertionPoint;
      if (aOther.mCaretPoint.IsSet()) {
        mCaretPoint = aOther.mCaretPoint;
      }
      return *this;
    }
    // If one of the result is NS_ERROR_EDITOR_DESTROYED, use it since it's
    // the most important error code for editor.
    if (EditorDestroyed() || aOther.EditorDestroyed()) {
      mRv = NS_ERROR_EDITOR_DESTROYED;
      mNextInsertionPoint.Clear();
      mCaretPoint.Clear();
      return *this;
    }
    // If the other one has not been set explicit nsresult, keep current
    // value.
    if (aOther.NotInitialized()) {
      return *this;
    }
    // If this one has not been set explicit nsresult, copy the other one's.
    if (NotInitialized()) {
      mRv = aOther.mRv;
      mNextInsertionPoint = aOther.mNextInsertionPoint;
      mCaretPoint = aOther.mCaretPoint;
      return *this;
    }
    // If one of the results is error, use NS_ERROR_FAILURE.
    if (isErr() || aOther.isErr()) {
      mRv = NS_ERROR_FAILURE;
      mNextInsertionPoint.Clear();
      mCaretPoint.Clear();
      return *this;
    }
    // Otherwise, use generic success code, NS_OK, and use newer point.
    mRv = NS_OK;
    mNextInsertionPoint = aOther.mNextInsertionPoint;
    if (aOther.mCaretPoint.IsSet()) {
      mCaretPoint = aOther.mCaretPoint;
    }
    return *this;
  }

#ifdef DEBUG
  ~MoveNodeResult() {
    MOZ_ASSERT_IF(isOk() && Handled(),
                  !mCaretPoint.IsSet() || mHandledCaretPoint);
  }
#endif

 private:
  explicit MoveNodeResult(const EditorDOMPoint& aNextInsertionPoint,
                          bool aHandled)
      : mNextInsertionPoint(aNextInsertionPoint),
        mRv(aNextInsertionPoint.IsSet() ? NS_OK : NS_ERROR_FAILURE),
        mHandled(aHandled && aNextInsertionPoint.IsSet()) {
    if (mNextInsertionPoint.IsSet()) {
      AutoEditorDOMPointChildInvalidator computeOffsetAndForgetChild(
          mNextInsertionPoint);
    }
  }
  explicit MoveNodeResult(EditorDOMPoint&& aNextInsertionPoint, bool aHandled)
      : mNextInsertionPoint(std::move(aNextInsertionPoint)),
        mRv(mNextInsertionPoint.IsSet() ? NS_OK : NS_ERROR_FAILURE),
        mHandled(aHandled && mNextInsertionPoint.IsSet()) {
    if (mNextInsertionPoint.IsSet()) {
      AutoEditorDOMPointChildInvalidator computeOffsetAndForgetChild(
          mNextInsertionPoint);
    }
  }
  explicit MoveNodeResult(const EditorDOMPoint& aNextInsertionPoint,
                          const EditorDOMPoint& aPointToPutCaret)
      : mNextInsertionPoint(aNextInsertionPoint),
        mCaretPoint(aPointToPutCaret),
        mRv(mNextInsertionPoint.IsSet() ? NS_OK : NS_ERROR_FAILURE),
        mHandled(mNextInsertionPoint.IsSet()) {
    if (mNextInsertionPoint.IsSet()) {
      AutoEditorDOMPointChildInvalidator computeOffsetAndForgetChild(
          mNextInsertionPoint);
    }
  }
  explicit MoveNodeResult(EditorDOMPoint&& aNextInsertionPoint,
                          const EditorDOMPoint& aPointToPutCaret)
      : mNextInsertionPoint(std::move(aNextInsertionPoint)),
        mCaretPoint(aPointToPutCaret),
        mRv(mNextInsertionPoint.IsSet() ? NS_OK : NS_ERROR_FAILURE),
        mHandled(mNextInsertionPoint.IsSet()) {
    if (mNextInsertionPoint.IsSet()) {
      AutoEditorDOMPointChildInvalidator computeOffsetAndForgetChild(
          mNextInsertionPoint);
    }
  }
  explicit MoveNodeResult(const EditorDOMPoint& aNextInsertionPoint,
                          EditorDOMPoint&& aPointToPutCaret)
      : mNextInsertionPoint(aNextInsertionPoint),
        mCaretPoint(std::move(aPointToPutCaret)),
        mRv(mNextInsertionPoint.IsSet() ? NS_OK : NS_ERROR_FAILURE),
        mHandled(mNextInsertionPoint.IsSet()) {
    if (mNextInsertionPoint.IsSet()) {
      AutoEditorDOMPointChildInvalidator computeOffsetAndForgetChild(
          mNextInsertionPoint);
    }
  }
  explicit MoveNodeResult(EditorDOMPoint&& aNextInsertionPoint,
                          EditorDOMPoint&& aPointToPutCaret)
      : mNextInsertionPoint(std::move(aNextInsertionPoint)),
        mCaretPoint(std::move(aPointToPutCaret)),
        mRv(mNextInsertionPoint.IsSet() ? NS_OK : NS_ERROR_FAILURE),
        mHandled(mNextInsertionPoint.IsSet()) {
    if (mNextInsertionPoint.IsSet()) {
      AutoEditorDOMPointChildInvalidator computeOffsetAndForgetChild(
          mNextInsertionPoint);
    }
  }

  EditorDOMPoint mNextInsertionPoint;
  // Recommended caret point after moving a node.
  EditorDOMPoint mCaretPoint;
  nsresult mRv;
  bool mHandled;
  bool mutable mHandledCaretPoint = false;

  friend MoveNodeResult MoveNodeIgnored(
      const EditorDOMPoint& aNextInsertionPoint);
  friend MoveNodeResult MoveNodeIgnored(EditorDOMPoint&& aNextInsertionPoint);
  friend MoveNodeResult MoveNodeHandled(
      const EditorDOMPoint& aNextInsertionPoint);
  friend MoveNodeResult MoveNodeHandled(EditorDOMPoint&& aNextInsertionPoint);
  friend MoveNodeResult MoveNodeHandled(
      const EditorDOMPoint& aNextInsertionPoint,
      const EditorDOMPoint& aPointToPutCaret);
  friend MoveNodeResult MoveNodeHandled(EditorDOMPoint&& aNextInsertionPoint,
                                        const EditorDOMPoint& aPointToPutCaret);
  friend MoveNodeResult MoveNodeHandled(
      const EditorDOMPoint& aNextInsertionPoint,
      EditorDOMPoint&& aPointToPutCaret);
  friend MoveNodeResult MoveNodeHandled(EditorDOMPoint&& aNextInsertionPoint,
                                        EditorDOMPoint&& aPointToPutCaret);
};

/*****************************************************************************
 * When a move node handler (or its helper) does nothing,
 * MoveNodeIgnored should be returned.
 *****************************************************************************/
inline MoveNodeResult MoveNodeIgnored(
    const EditorDOMPoint& aNextInsertionPoint) {
  return MoveNodeResult(aNextInsertionPoint, false);
}

inline MoveNodeResult MoveNodeIgnored(EditorDOMPoint&& aNextInsertionPoint) {
  return MoveNodeResult(std::move(aNextInsertionPoint), false);
}

/*****************************************************************************
 * When a move node handler (or its helper) handled and not canceled,
 * MoveNodeHandled should be returned.
 *****************************************************************************/
inline MoveNodeResult MoveNodeHandled(
    const EditorDOMPoint& aNextInsertionPoint) {
  return MoveNodeResult(aNextInsertionPoint, true);
}

inline MoveNodeResult MoveNodeHandled(EditorDOMPoint&& aNextInsertionPoint) {
  return MoveNodeResult(std::move(aNextInsertionPoint), true);
}

inline MoveNodeResult MoveNodeHandled(const EditorDOMPoint& aNextInsertionPoint,
                                      const EditorDOMPoint& aPointToPutCaret) {
  return MoveNodeResult(aNextInsertionPoint, aPointToPutCaret);
}

inline MoveNodeResult MoveNodeHandled(EditorDOMPoint&& aNextInsertionPoint,
                                      const EditorDOMPoint& aPointToPutCaret) {
  return MoveNodeResult(std::move(aNextInsertionPoint), aPointToPutCaret);
}

inline MoveNodeResult MoveNodeHandled(const EditorDOMPoint& aNextInsertionPoint,
                                      EditorDOMPoint&& aPointToPutCaret) {
  return MoveNodeResult(aNextInsertionPoint, std::move(aPointToPutCaret));
}

inline MoveNodeResult MoveNodeHandled(EditorDOMPoint&& aNextInsertionPoint,
                                      EditorDOMPoint&& aPointToPutCaret) {
  return MoveNodeResult(std::move(aNextInsertionPoint),
                        std::move(aPointToPutCaret));
}

/*****************************************************************************
 * SplitNodeResult is a simple class for
 * HTMLEditor::SplitNodeDeepWithTransaction().
 * This makes the callers' code easier to read.
 * TODO: Perhaps, we can make this inherits mozilla::Result for guaranteeing
 *       same API.  Then, changing to/from Result<*, nsresult> can be easier.
 *       For now, we should give same API name rather than same as
 *       mozilla::ErrorResult.
 *****************************************************************************/
class MOZ_STACK_CLASS SplitNodeResult final {
 public:
  // FYI: NS_SUCCEEDED and NS_FAILED contain MOZ_(UN)LIKELY so that isOk() and
  //      isErr() must not required to wrap with them.
  bool isOk() const { return NS_SUCCEEDED(mRv); }
  bool isErr() const { return NS_FAILED(mRv); }
  constexpr nsresult inspectErr() const { return mRv; }
  constexpr nsresult unwrapErr() const { return inspectErr(); }
  bool Handled() const { return mPreviousNode || mNextNode; }
  constexpr bool EditorDestroyed() const {
    return MOZ_UNLIKELY(mRv == NS_ERROR_EDITOR_DESTROYED);
  }

  /**
   * DidSplit() returns true if a node was actually split.
   */
  bool DidSplit() const { return mPreviousNode && mNextNode; }

  /**
   * GetPreviousContent() returns previous content node at the split point.
   */
  MOZ_KNOWN_LIVE nsIContent* GetPreviousContent() const {
    MOZ_ASSERT(isOk());
    if (mGivenSplitPoint.IsSet()) {
      return mGivenSplitPoint.IsEndOfContainer() ? mGivenSplitPoint.GetChild()
                                                 : nullptr;
    }
    return mPreviousNode;
  }
  template <typename EditorDOMPointType>
  EditorDOMPointType AtPreviousContent() const {
    if (nsIContent* previousContent = GetPreviousContent()) {
      return EditorDOMPointType(previousContent);
    }
    return EditorDOMPointType();
  }

  /**
   * GetNextContent() returns next content node at the split point.
   */
  MOZ_KNOWN_LIVE nsIContent* GetNextContent() const {
    MOZ_ASSERT(isOk());
    if (mGivenSplitPoint.IsSet()) {
      return !mGivenSplitPoint.IsEndOfContainer() ? mGivenSplitPoint.GetChild()
                                                  : nullptr;
    }
    return mNextNode;
  }
  template <typename EditorDOMPointType>
  EditorDOMPointType AtNextContent() const {
    if (nsIContent* nextContent = GetNextContent()) {
      return EditorDOMPointType(nextContent);
    }
    return EditorDOMPointType();
  }

  /**
   * Returns new content node which is created at splitting a node.  I.e., this
   * returns nullptr if no node was split.
   */
  MOZ_KNOWN_LIVE nsIContent* GetNewContent() const {
    MOZ_ASSERT(isOk());
    if (!DidSplit()) {
      return nullptr;
    }
    return mDirection == SplitNodeDirection::LeftNodeIsNewOne ? mPreviousNode
                                                              : mNextNode;
  }
  template <typename EditorDOMPointType>
  EditorDOMPointType AtNewContent() const {
    if (nsIContent* newContent = GetNewContent()) {
      return EditorDOMPointType(newContent);
    }
    return EditorDOMPointType();
  }

  /**
   * Returns original content node which is (or is just tried to be) split.
   */
  MOZ_KNOWN_LIVE nsIContent* GetOriginalContent() const {
    MOZ_ASSERT(isOk());
    if (mGivenSplitPoint.IsSet()) {
      // Different from previous/next content, if the creator didn't split a
      // node, the container of the split point is the original node.
      return mGivenSplitPoint.GetContainerAs<nsIContent>();
    }
    if (mDirection == SplitNodeDirection::LeftNodeIsNewOne) {
      return mNextNode ? mNextNode : mPreviousNode;
    }
    return mPreviousNode ? mPreviousNode : mNextNode;
  }
  template <typename EditorDOMPointType>
  EditorDOMPointType AtOriginalContent() const {
    if (nsIContent* originalContent = GetOriginalContent()) {
      return EditorDOMPointType(originalContent);
    }
    return EditorDOMPointType();
  }

  /**
   * AtSplitPoint() returns the split point in the container.
   * HTMLEditor::CreateAndInsertElement() or something similar methods.
   */
  template <typename EditorDOMPointType>
  EditorDOMPointType AtSplitPoint() const {
    if (isErr()) {
      return EditorDOMPointType();
    }
    if (mGivenSplitPoint.IsSet()) {
      return mGivenSplitPoint.To<EditorDOMPointType>();
    }
    if (!mPreviousNode) {
      return EditorDOMPointType(mNextNode);
    }
    return EditorDOMPointType::After(mPreviousNode);
  }

  /**
   * Suggest caret position to aHTMLEditor.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult SuggestCaretPointTo(
      const HTMLEditor& aHTMLEditor, const SuggestCaretOptions& aOptions) const;

  /**
   * IgnoreCaretPointSuggestion() should be called if the method does not want
   * to use caret position recommended by this instance.
   */
  void IgnoreCaretPointSuggestion() const { mHandledCaretPoint = true; }

  bool HasCaretPointSuggestion() const { return mCaretPoint.IsSet(); }
  constexpr EditorDOMPoint&& UnwrapCaretPoint() {
    mHandledCaretPoint = true;
    return std::move(mCaretPoint);
  }
  bool MoveCaretPointTo(EditorDOMPoint& aPointToPutCaret,
                        const SuggestCaretOptions& aOptions) {
    MOZ_ASSERT(!aOptions.contains(SuggestCaret::AndIgnoreTrivialError));
    MOZ_ASSERT(
        !aOptions.contains(SuggestCaret::OnlyIfTransactionsAllowedToDoIt));
    if (aOptions.contains(SuggestCaret::OnlyIfHasSuggestion) &&
        !mCaretPoint.IsSet()) {
      return false;
    }
    aPointToPutCaret = UnwrapCaretPoint();
    return true;
  }
  bool MoveCaretPointTo(EditorDOMPoint& aPointToPutCaret,
                        const HTMLEditor& aHTMLEditor,
                        const SuggestCaretOptions& aOptions);

  SplitNodeResult() = delete;
  SplitNodeResult(const SplitNodeResult&) = delete;
  SplitNodeResult& operator=(const SplitNodeResult&) = delete;
  SplitNodeResult(SplitNodeResult&&) = default;
  SplitNodeResult& operator=(SplitNodeResult&&) = default;

  /**
   * This constructor should be used for setting specific caret point instead of
   * aSplitResult's one.
   */
  SplitNodeResult(SplitNodeResult&& aSplitResult,
                  const EditorDOMPoint& aNewCaretPoint)
      : SplitNodeResult(std::move(aSplitResult)) {
    MOZ_ASSERT(isOk());
    mCaretPoint = aNewCaretPoint;
    mHandledCaretPoint = false;
  }
  SplitNodeResult(SplitNodeResult&& aSplitResult,
                  EditorDOMPoint&& aNewCaretPoint)
      : SplitNodeResult(std::move(aSplitResult)) {
    MOZ_ASSERT(isOk());
    mCaretPoint = std::move(aNewCaretPoint);
    mHandledCaretPoint = false;
  }

  /**
   * This constructor shouldn't be used by anybody except methods which
   * use this as result when it succeeds.
   *
   * @param aNewNode    The node which is newly created.
   * @param aSplitNode  The node which was split.
   * @param aDirection  The split direction which the HTML editor tried to split
   *                    a node with.
   * @param aNewCaretPoint
   *                    An optional new caret position.  If this is omitted,
   *                    the point between new node and split node will be
   *                    suggested.
   */
  SplitNodeResult(nsIContent& aNewNode, nsIContent& aSplitNode,
                  SplitNodeDirection aDirection,
                  const Maybe<EditorDOMPoint>& aNewCaretPoint = Nothing())
      : mPreviousNode(aDirection == SplitNodeDirection::LeftNodeIsNewOne
                          ? &aNewNode
                          : &aSplitNode),
        mNextNode(aDirection == SplitNodeDirection::LeftNodeIsNewOne
                      ? &aSplitNode
                      : &aNewNode),
        mCaretPoint(aNewCaretPoint.isSome()
                        ? aNewCaretPoint.ref()
                        : EditorDOMPoint::AtEndOf(mPreviousNode)),
        mRv(NS_OK),
        mDirection(aDirection) {}
  SplitNodeResult(nsCOMPtr<nsIContent>&& aNewNode,
                  nsCOMPtr<nsIContent>&& aSplitNode,
                  SplitNodeDirection aDirection,
                  const Maybe<EditorDOMPoint>& aNewCaretPoint = Nothing())
      : mPreviousNode(aDirection == SplitNodeDirection::LeftNodeIsNewOne
                          ? std::move(aNewNode)
                          : std::move(aSplitNode)),
        mNextNode(aDirection == SplitNodeDirection::LeftNodeIsNewOne
                      ? std::move(aSplitNode)
                      : std::move(aNewNode)),
        mCaretPoint(aNewCaretPoint.isSome()
                        ? aNewCaretPoint.ref()
                        : (MOZ_LIKELY(mPreviousNode)
                               ? EditorDOMPoint::AtEndOf(mPreviousNode)
                               : EditorDOMPoint())),
        mRv(NS_OK),
        mDirection(aDirection) {
    MOZ_DIAGNOSTIC_ASSERT(mPreviousNode);
    MOZ_DIAGNOSTIC_ASSERT(mNextNode);
  }

  /**
   * This constructor shouldn't be used by anybody except methods which
   * use this as error result when it fails.
   */
  explicit SplitNodeResult(nsresult aRv)
      : mRv(aRv), mDirection(SplitNodeDirection::LeftNodeIsNewOne) {
    MOZ_DIAGNOSTIC_ASSERT(NS_FAILED(mRv));
  }

  SplitNodeResult ToHandledResult() const {
    mHandledCaretPoint = true;
    SplitNodeResult result(NS_OK, mDirection);
    result.mPreviousNode = GetPreviousContent();
    result.mNextNode = GetNextContent();
    MOZ_DIAGNOSTIC_ASSERT(result.Handled());
    // Don't recompute the caret position because in this case, split has not
    // occurred yet.  In the case,  the caller shouldn't need to update
    // selection.
    result.mCaretPoint = mCaretPoint;
    return result;
  }

  /**
   * The following factory methods creates a SplitNodeResult instance for the
   * special cases.
   *
   * @param aDeeperSplitNodeResult
   *                    If the splitter has already split a child or a
   *                    descendant of the latest split node, the split node
   *                    result should be specified.
   */
  static inline SplitNodeResult HandledButDidNotSplitDueToEndOfContainer(
      nsIContent& aNotSplitNode, SplitNodeDirection aDirection,
      const SplitNodeResult* aDeeperSplitNodeResult = nullptr) {
    SplitNodeResult result(NS_OK, aDirection);
    result.mPreviousNode = &aNotSplitNode;
    // Caret should be put at the last split point instead of current node.
    if (aDeeperSplitNodeResult) {
      result.mCaretPoint = aDeeperSplitNodeResult->mCaretPoint;
      aDeeperSplitNodeResult->mHandledCaretPoint = true;
    }
    return result;
  }

  static inline SplitNodeResult HandledButDidNotSplitDueToStartOfContainer(
      nsIContent& aNotSplitNode, SplitNodeDirection aDirection,
      const SplitNodeResult* aDeeperSplitNodeResult = nullptr) {
    SplitNodeResult result(NS_OK, aDirection);
    result.mNextNode = &aNotSplitNode;
    // Caret should be put at the last split point instead of current node.
    if (aDeeperSplitNodeResult) {
      result.mCaretPoint = aDeeperSplitNodeResult->mCaretPoint;
      aDeeperSplitNodeResult->mHandledCaretPoint = true;
    }
    return result;
  }

  template <typename PT, typename CT>
  static inline SplitNodeResult NotHandled(
      const EditorDOMPointBase<PT, CT>& aGivenSplitPoint,
      SplitNodeDirection aDirection,
      const SplitNodeResult* aDeeperSplitNodeResult = nullptr) {
    SplitNodeResult result(NS_OK, aDirection);
    result.mGivenSplitPoint = aGivenSplitPoint;
    // Caret should be put at the last split point instead of current node.
    if (aDeeperSplitNodeResult) {
      result.mCaretPoint = aDeeperSplitNodeResult->mCaretPoint;
      aDeeperSplitNodeResult->mHandledCaretPoint = true;
    }
    return result;
  }

  /**
   * Returns aSplitNodeResult as-is unless it didn't split a node but
   * aDeeperSplitNodeResult has already split a child or a descendant and has a
   * valid point to put caret around there.  In the case, this return
   * aSplitNodeResult which suggests a caret position around the last split
   * point.
   */
  static inline SplitNodeResult MergeWithDeeperSplitNodeResult(
      SplitNodeResult&& aSplitNodeResult,
      const SplitNodeResult& aDeeperSplitNodeResult) {
    aSplitNodeResult.mHandledCaretPoint = false;
    aDeeperSplitNodeResult.mHandledCaretPoint = true;
    if (aSplitNodeResult.DidSplit() ||
        !aDeeperSplitNodeResult.mCaretPoint.IsSet()) {
      return std::move(aSplitNodeResult);
    }
    SplitNodeResult result(std::move(aSplitNodeResult));
    result.mCaretPoint = aDeeperSplitNodeResult.mCaretPoint;
    return result;
  }

#ifdef DEBUG
  ~SplitNodeResult() {
    MOZ_ASSERT_IF(isOk(), !mCaretPoint.IsSet() || mHandledCaretPoint);
  }
#endif

 private:
  SplitNodeResult(nsresult aRv, SplitNodeDirection aDirection)
      : mRv(aRv), mDirection(aDirection) {}

  // When methods which return this class split some nodes actually, they
  // need to set a set of left node and right node to this class.  However,
  // one or both of them may be moved or removed by mutation observer.
  // In such case, we cannot represent the point with EditorDOMPoint since
  // it requires current container node.  Therefore, we need to use
  // nsCOMPtr<nsIContent> here instead.
  nsCOMPtr<nsIContent> mPreviousNode;
  nsCOMPtr<nsIContent> mNextNode;

  // Methods which return this class may not split any nodes actually.  Then,
  // they may want to return given split point as is since such behavior makes
  // their callers simpler.  In this case, the point may be in a text node
  // which cannot be represented as a node.  Therefore, we need EditorDOMPoint
  // for representing the point.
  EditorDOMPoint mGivenSplitPoint;

  // The point which is a good point to put caret from point of view the
  // splitter.
  EditorDOMPoint mCaretPoint;

  nsresult mRv;
  SplitNodeDirection mDirection;
  bool mutable mHandledCaretPoint = false;
};

/*****************************************************************************
 * JoinNodesResult is a simple class for HTMLEditor::JoinNodesWithTransaction().
 * This makes the callers' code easier to read.
 *****************************************************************************/
class MOZ_STACK_CLASS JoinNodesResult final {
 public:
  bool Succeeded() const { return NS_SUCCEEDED(mRv); }
  bool Failed() const { return NS_FAILED(mRv); }
  nsresult Rv() const { return mRv; }
  bool Handled() const { return Succeeded(); }
  bool EditorDestroyed() const { return mRv == NS_ERROR_EDITOR_DESTROYED; }

  MOZ_KNOWN_LIVE nsIContent* ExistingContent() const {
    MOZ_ASSERT(Succeeded());
    return mJoinedPoint.ContainerAs<nsIContent>();
  }
  template <typename EditorDOMPointType>
  EditorDOMPointType AtExistingContent() const {
    MOZ_ASSERT(Succeeded());
    return EditorDOMPointType(mJoinedPoint.ContainerAs<nsIContent>());
  }

  MOZ_KNOWN_LIVE nsIContent* RemovedContent() const {
    MOZ_ASSERT(Succeeded());
    return mRemovedContent;
  }
  template <typename EditorDOMPointType>
  EditorDOMPointType AtRemovedContent() const {
    MOZ_ASSERT(Succeeded());
    if (mRemovedContent) {
      return EditorDOMPointType(mRemovedContent);
    }
    return EditorDOMPointType();
  }

  template <typename EditorDOMPointType>
  EditorDOMPointType AtJoinedPoint() const {
    MOZ_ASSERT(Succeeded());
    return mJoinedPoint;
  }

  JoinNodesResult() = delete;

  /**
   * This constructor shouldn't be used by anybody except methods which
   * use this as result when it succeeds.
   *
   * @param aJoinedPoint        First child of right node or first character.
   * @param aRemovedContent     The node which was removed from the parent.
   * @param aDirection          The join direction which the HTML editor tried
   *                            to join the nodes with.
   */
  JoinNodesResult(const EditorDOMPoint& aJoinedPoint,
                  nsIContent& aRemovedContent, JoinNodesDirection aDirection)
      : mJoinedPoint(aJoinedPoint),
        mRemovedContent(&aRemovedContent),
        mRv(NS_OK) {
    MOZ_DIAGNOSTIC_ASSERT(aJoinedPoint.IsInContentNode());
  }

  /**
   * This constructor shouldn't be used by anybody except methods which
   * use this as error result when it fails.
   */
  explicit JoinNodesResult(nsresult aRv) : mRv(aRv) {
    MOZ_DIAGNOSTIC_ASSERT(NS_FAILED(mRv));
  }

 private:
  EditorDOMPoint mJoinedPoint;
  nsCOMPtr<nsIContent> mRemovedContent;

  nsresult mRv;
};

/*****************************************************************************
 * SplitRangeOffFromNodeResult class is a simple class for methods which split a
 * node at 2 points for making part of the node split off from the node.
 * TODO: Perhaps, we can make this inherits mozilla::Result for guaranteeing
 *       same API.  Then, changing to/from Result<*, nsresult> can be easier.
 *       For now, we should give same API name rather than same as
 *       mozilla::ErrorResult.
 *****************************************************************************/
class MOZ_STACK_CLASS SplitRangeOffFromNodeResult final {
 public:
  // FYI: NS_SUCCEEDED and NS_FAILED contain MOZ_(UN)LIKELY so that isOk() and
  //      isErr() must not required to wrap with them.
  bool isOk() const { return NS_SUCCEEDED(mRv); }
  bool isErr() const { return NS_FAILED(mRv); }
  constexpr nsresult inspectErr() const { return mRv; }
  constexpr nsresult unwrapErr() const { return inspectErr(); }
  constexpr bool EditorDestroyed() const {
    return MOZ_UNLIKELY(mRv == NS_ERROR_EDITOR_DESTROYED);
  }

  /**
   * GetLeftContent() returns new created node before the part of quarried out.
   * This may return nullptr if the method didn't split at start edge of
   * the node.
   */
  MOZ_KNOWN_LIVE nsIContent* GetLeftContent() const { return mLeftContent; }
  template <typename ContentNodeType>
  MOZ_KNOWN_LIVE ContentNodeType* GetLeftContentAs() const {
    return ContentNodeType::FromNodeOrNull(GetLeftContent());
  }

  /**
   * GetMiddleContent() returns new created node between left node and right
   * node.  I.e., this is quarried out from the node.  This may return nullptr
   * if the method unwrapped the middle node.
   */
  MOZ_KNOWN_LIVE nsIContent* GetMiddleContent() const { return mMiddleContent; }
  template <typename ContentNodeType>
  MOZ_KNOWN_LIVE ContentNodeType* GetMiddleContentAs() const {
    return ContentNodeType::FromNodeOrNull(GetMiddleContent());
  }

  /**
   * GetRightContent() returns the right node after the part of quarried out.
   * This may return nullptr it the method didn't split at end edge of the
   * node.
   */
  MOZ_KNOWN_LIVE nsIContent* GetRightContent() const { return mRightContent; }
  template <typename ContentNodeType>
  MOZ_KNOWN_LIVE ContentNodeType* GetRightContentAs() const {
    return ContentNodeType::FromNodeOrNull(GetRightContent());
  }

  /**
   * GetLeftmostContent() returns the leftmost content after trying to
   * split twice.  If the node was not split, this returns the original node.
   */
  MOZ_KNOWN_LIVE nsIContent* GetLeftmostContent() const {
    return mLeftContent ? mLeftContent
                        : (mMiddleContent ? mMiddleContent : mRightContent);
  }
  template <typename ContentNodeType>
  MOZ_KNOWN_LIVE ContentNodeType* GetLeftmostContentAs() const {
    return ContentNodeType::FromNodeOrNull(GetLeftmostContent());
  }

  /**
   * GetRightmostContent() returns the rightmost content after trying to
   * split twice.  If the node was not split, this returns the original node.
   */
  MOZ_KNOWN_LIVE nsIContent* GetRightmostContent() const {
    return mRightContent ? mRightContent
                         : (mMiddleContent ? mMiddleContent : mLeftContent);
  }
  template <typename ContentNodeType>
  MOZ_KNOWN_LIVE ContentNodeType* GetRightmostContentAs() const {
    return ContentNodeType::FromNodeOrNull(GetRightmostContent());
  }

  /**
   * Suggest caret position to aHTMLEditor.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult SuggestCaretPointTo(
      const HTMLEditor& aHTMLEditor, const SuggestCaretOptions& aOptions) const;

  /**
   * IgnoreCaretPointSuggestion() should be called if the method does not want
   * to use caret position recommended by this instance.
   */
  void IgnoreCaretPointSuggestion() const { mHandledCaretPoint = true; }

  bool HasCaretPointSuggestion() const { return mCaretPoint.IsSet(); }
  constexpr EditorDOMPoint&& UnwrapCaretPoint() {
    mHandledCaretPoint = true;
    return std::move(mCaretPoint);
  }
  bool MoveCaretPointTo(EditorDOMPoint& aPointToPutCaret,
                        const SuggestCaretOptions& aOptions) {
    MOZ_ASSERT(!aOptions.contains(SuggestCaret::AndIgnoreTrivialError));
    MOZ_ASSERT(
        !aOptions.contains(SuggestCaret::OnlyIfTransactionsAllowedToDoIt));
    if (aOptions.contains(SuggestCaret::OnlyIfHasSuggestion) &&
        !mCaretPoint.IsSet()) {
      return false;
    }
    aPointToPutCaret = UnwrapCaretPoint();
    return true;
  }
  bool MoveCaretPointTo(EditorDOMPoint& aPointToPutCaret,
                        const HTMLEditor& aHTMLEditor,
                        const SuggestCaretOptions& aOptions);

  SplitRangeOffFromNodeResult() = delete;

  SplitRangeOffFromNodeResult(nsIContent* aLeftContent,
                              nsIContent* aMiddleContent,
                              nsIContent* aRightContent)
      : mLeftContent(aLeftContent),
        mMiddleContent(aMiddleContent),
        mRightContent(aRightContent),
        mRv(NS_OK) {}

  SplitRangeOffFromNodeResult(nsIContent* aLeftContent,
                              nsIContent* aMiddleContent,
                              nsIContent* aRightContent,
                              EditorDOMPoint&& aPointToPutCaret)
      : mLeftContent(aLeftContent),
        mMiddleContent(aMiddleContent),
        mRightContent(aRightContent),
        mCaretPoint(std::move(aPointToPutCaret)),
        mRv(NS_OK) {}

  explicit SplitRangeOffFromNodeResult(nsresult aRv) : mRv(aRv) {
    MOZ_DIAGNOSTIC_ASSERT(NS_FAILED(mRv));
  }

  SplitRangeOffFromNodeResult(const SplitRangeOffFromNodeResult& aOther) =
      delete;
  SplitRangeOffFromNodeResult& operator=(
      const SplitRangeOffFromNodeResult& aOther) = delete;
  SplitRangeOffFromNodeResult(SplitRangeOffFromNodeResult&& aOther) = default;
  SplitRangeOffFromNodeResult& operator=(SplitRangeOffFromNodeResult&& aOther) =
      default;

#ifdef DEBUG
  ~SplitRangeOffFromNodeResult() {
    MOZ_ASSERT_IF(isOk(), !mCaretPoint.IsSet() || mHandledCaretPoint);
  }
#endif

 private:
  MOZ_KNOWN_LIVE nsCOMPtr<nsIContent> mLeftContent;
  MOZ_KNOWN_LIVE nsCOMPtr<nsIContent> mMiddleContent;
  MOZ_KNOWN_LIVE nsCOMPtr<nsIContent> mRightContent;

  // The point which is a good point to put caret from point of view the
  // splitter.
  EditorDOMPoint mCaretPoint;

  nsresult mRv;

  bool mutable mHandledCaretPoint = false;
};

/*****************************************************************************
 * SplitRangeOffResult class is a simple class for methods which splits
 * specific ancestor elements at 2 DOM points.
 * TODO: Perhaps, we can make this inherits mozilla::Result for guaranteeing
 *       same API.  Then, changing to/from Result<*, nsresult> can be easier.
 *       For now, we should give same API name rather than same as
 *       mozilla::ErrorResult.
 *****************************************************************************/
class MOZ_STACK_CLASS SplitRangeOffResult final {
 public:
  // FYI: NS_SUCCEEDED and NS_FAILED contain MOZ_(UN)LIKELY so that isOk() and
  //      isErr() must not required to wrap with them.
  bool isOk() const { return NS_SUCCEEDED(mRv); }
  bool isErr() const { return NS_FAILED(mRv); }
  constexpr nsresult inspectErr() const { return mRv; }
  constexpr nsresult unwrapErr() const { return inspectErr(); }
  constexpr bool Handled() const { return mHandled; }
  constexpr bool EditorDestroyed() const {
    return MOZ_UNLIKELY(mRv == NS_ERROR_EDITOR_DESTROYED);
  }

  /**
   * The start boundary is at the right of split at split point.  The end
   * boundary is at right node of split at end point, i.e., the end boundary
   * points out of the range to have been split off.
   */
  constexpr const EditorDOMRange& RangeRef() const { return mRange; }

  /**
   * Suggest caret position to aHTMLEditor.
   */
  [[nodiscard]] MOZ_CAN_RUN_SCRIPT nsresult SuggestCaretPointTo(
      const HTMLEditor& aHTMLEditor, const SuggestCaretOptions& aOptions) const;

  /**
   * IgnoreCaretPointSuggestion() should be called if the method does not want
   * to use caret position recommended by this instance.
   */
  void IgnoreCaretPointSuggestion() const { mHandledCaretPoint = true; }

  bool HasCaretPointSuggestion() const { return mCaretPoint.IsSet(); }
  constexpr EditorDOMPoint&& UnwrapCaretPoint() {
    mHandledCaretPoint = true;
    return std::move(mCaretPoint);
  }
  bool MoveCaretPointTo(EditorDOMPoint& aPointToPutCaret,
                        const SuggestCaretOptions& aOptions) {
    MOZ_ASSERT(!aOptions.contains(SuggestCaret::AndIgnoreTrivialError));
    MOZ_ASSERT(
        !aOptions.contains(SuggestCaret::OnlyIfTransactionsAllowedToDoIt));
    if (aOptions.contains(SuggestCaret::OnlyIfHasSuggestion) &&
        !mCaretPoint.IsSet()) {
      return false;
    }
    aPointToPutCaret = UnwrapCaretPoint();
    return true;
  }
  bool MoveCaretPointTo(EditorDOMPoint& aPointToPutCaret,
                        const HTMLEditor& aHTMLEditor,
                        const SuggestCaretOptions& aOptions);

  SplitRangeOffResult() = delete;

  /**
   * Constructor for success case.
   *
   * @param aTrackedRangeStart          The range whose start is at topmost
   *                                    right node child at start point if
   *                                    actually split there, or at the point
   *                                    to be tried to split, and whose end is
   *                                    at topmost right node child at end point
   *                                    if actually split there, or at the point
   *                                    to be tried to split.  Note that if the
   *                                    method allows to run script after
   *                                    splitting the range boundaries, they
   *                                    should be tracked with
   *                                    AutoTrackDOMRange.
   * @param aSplitNodeResultAtStart     Raw split node result at start point.
   * @param aSplitNodeResultAtEnd       Raw split node result at start point.
   */
  SplitRangeOffResult(EditorDOMRange&& aTrackedRange,
                      SplitNodeResult&& aSplitNodeResultAtStart,
                      SplitNodeResult&& aSplitNodeResultAtEnd)
      : mRange(std::move(aTrackedRange)),
        mRv(NS_OK),
        mHandled(aSplitNodeResultAtStart.Handled() ||
                 aSplitNodeResultAtEnd.Handled()) {
    MOZ_ASSERT(mRange.StartRef().IsSet());
    MOZ_ASSERT(mRange.EndRef().IsSet());
    MOZ_ASSERT(aSplitNodeResultAtStart.isOk());
    MOZ_ASSERT(aSplitNodeResultAtEnd.isOk());
    // The given results are created for creating this instance so that the
    // caller may not need to handle with them.  For making who taking the
    // responsible clearer, we should move them into this constructor.
    SplitNodeResult splitNodeResultAtStart(std::move(aSplitNodeResultAtStart));
    SplitNodeResult splitNodeResultAtEnd(std::move(aSplitNodeResultAtEnd));
    splitNodeResultAtStart.MoveCaretPointTo(
        mCaretPoint, {SuggestCaret::OnlyIfHasSuggestion});
    splitNodeResultAtEnd.MoveCaretPointTo(mCaretPoint,
                                          {SuggestCaret::OnlyIfHasSuggestion});
  }

  explicit SplitRangeOffResult(nsresult aRv) : mRv(aRv), mHandled(false) {
    MOZ_DIAGNOSTIC_ASSERT(NS_FAILED(mRv));
  }

  SplitRangeOffResult(const SplitRangeOffResult& aOther) = delete;
  SplitRangeOffResult& operator=(const SplitRangeOffResult& aOther) = delete;
  SplitRangeOffResult(SplitRangeOffResult&& aOther) = default;
  SplitRangeOffResult& operator=(SplitRangeOffResult&& aOther) = default;

 private:
  EditorDOMRange mRange;

  // If you need to store previous and/or next node at start/end point,
  // you might be able to use `SplitNodeResult::GetPreviousNode()` etc in the
  // constructor only when `SplitNodeResult::Handled()` returns true.  But
  // the node might have gone with another DOM tree mutation.  So, be careful
  // if you do it.

  // The point which is a good point to put caret from point of view the
  // splitter.
  EditorDOMPoint mCaretPoint;

  nsresult mRv;

  bool mHandled;
  bool mutable mHandledCaretPoint = false;
};

/******************************************************************************
 * DOM tree iterators
 *****************************************************************************/

class MOZ_RAII DOMIterator {
 public:
  explicit DOMIterator();
  explicit DOMIterator(nsINode& aNode);
  virtual ~DOMIterator() = default;

  nsresult Init(nsRange& aRange);
  nsresult Init(const RawRangeBoundary& aStartRef,
                const RawRangeBoundary& aEndRef);

  template <class NodeClass>
  void AppendAllNodesToArray(
      nsTArray<OwningNonNull<NodeClass>>& aArrayOfNodes) const;

  /**
   * AppendNodesToArray() calls aFunctor before appending found node to
   * aArrayOfNodes.  If aFunctor returns false, the node will be ignored.
   * You can use aClosure instead of capturing something with lambda.
   * Note that aNode is guaranteed that it's an instance of NodeClass
   * or its sub-class.
   * XXX If we can make type of aNode templated without std::function,
   *     it'd be better, though.
   */
  typedef bool (*BoolFunctor)(nsINode& aNode, void* aClosure);
  template <class NodeClass>
  void AppendNodesToArray(BoolFunctor aFunctor,
                          nsTArray<OwningNonNull<NodeClass>>& aArrayOfNodes,
                          void* aClosure = nullptr) const;

 protected:
  ContentIteratorBase* mIter;
  PostContentIterator mPostOrderIter;
};

class MOZ_RAII DOMSubtreeIterator final : public DOMIterator {
 public:
  explicit DOMSubtreeIterator();
  virtual ~DOMSubtreeIterator() = default;

  nsresult Init(nsRange& aRange);

 private:
  ContentSubtreeIterator mSubtreeIter;
  explicit DOMSubtreeIterator(nsINode& aNode) = delete;
};

/******************************************************************************
 * ReplaceRangeData
 *
 * This represents range to be replaced and replacing string.
 *****************************************************************************/

template <typename EditorDOMPointType>
class MOZ_STACK_CLASS ReplaceRangeDataBase final {
 public:
  ReplaceRangeDataBase() = default;
  template <typename OtherEditorDOMRangeType>
  ReplaceRangeDataBase(const OtherEditorDOMRangeType& aRange,
                       const nsAString& aReplaceString)
      : mRange(aRange), mReplaceString(aReplaceString) {}
  template <typename StartPointType, typename EndPointType>
  ReplaceRangeDataBase(const StartPointType& aStart, const EndPointType& aEnd,
                       const nsAString& aReplaceString)
      : mRange(aStart, aEnd), mReplaceString(aReplaceString) {}

  bool IsSet() const { return mRange.IsPositioned(); }
  bool IsSetAndValid() const { return mRange.IsPositionedAndValid(); }
  bool Collapsed() const { return mRange.Collapsed(); }
  bool HasReplaceString() const { return !mReplaceString.IsEmpty(); }
  const EditorDOMPointType& StartRef() const { return mRange.StartRef(); }
  const EditorDOMPointType& EndRef() const { return mRange.EndRef(); }
  const EditorDOMRangeBase<EditorDOMPointType>& RangeRef() const {
    return mRange;
  }
  const nsString& ReplaceStringRef() const { return mReplaceString; }

  template <typename PointType>
  MOZ_NEVER_INLINE_DEBUG void SetStart(const PointType& aStart) {
    mRange.SetStart(aStart);
  }
  template <typename PointType>
  MOZ_NEVER_INLINE_DEBUG void SetEnd(const PointType& aEnd) {
    mRange.SetEnd(aEnd);
  }
  template <typename StartPointType, typename EndPointType>
  MOZ_NEVER_INLINE_DEBUG void SetStartAndEnd(const StartPointType& aStart,
                                             const EndPointType& aEnd) {
    mRange.SetRange(aStart, aEnd);
  }
  template <typename OtherEditorDOMRangeType>
  MOZ_NEVER_INLINE_DEBUG void SetRange(const OtherEditorDOMRangeType& aRange) {
    mRange = aRange;
  }
  void SetReplaceString(const nsAString& aReplaceString) {
    mReplaceString = aReplaceString;
  }
  template <typename StartPointType, typename EndPointType>
  MOZ_NEVER_INLINE_DEBUG void SetStartAndEnd(const StartPointType& aStart,
                                             const EndPointType& aEnd,
                                             const nsAString& aReplaceString) {
    SetStartAndEnd(aStart, aEnd);
    SetReplaceString(aReplaceString);
  }
  template <typename OtherEditorDOMRangeType>
  MOZ_NEVER_INLINE_DEBUG void Set(const OtherEditorDOMRangeType& aRange,
                                  const nsAString& aReplaceString) {
    SetRange(aRange);
    SetReplaceString(aReplaceString);
  }

 private:
  EditorDOMRangeBase<EditorDOMPointType> mRange;
  // This string may be used with ReplaceTextTransaction.  Therefore, for
  // avoiding memory copy, we should store it with nsString rather than
  // nsAutoString.
  nsString mReplaceString;
};

/******************************************************************************
 * EditorInlineStyle represents an inline style.
 ******************************************************************************/

struct MOZ_STACK_CLASS EditorInlineStyle {
  // nullptr if you want to remove all inline styles.
  // Otherwise, one of the presentation tag names which we support in style
  // editor, and there special cases: nsGkAtoms::href means <a href="...">,
  // and nsGkAtoms::name means <a name="...">.
  MOZ_KNOWN_LIVE nsStaticAtom* const mHTMLProperty = nullptr;
  // For some mHTMLProperty values, need to be set to its attribute name.
  // E.g., nsGkAtoms::size and nsGkAtoms::face for nsGkAtoms::font.
  // Otherwise, nullptr.
  // TODO: Once we stop using these structure to wrap selected content nodes
  //       with <a href> elements, we can make this nsStaticAtom*.
  MOZ_KNOWN_LIVE const RefPtr<nsAtom> mAttribute;

  /**
   * Returns true if the style means that all inline styles should be removed.
   */
  bool IsStyleToClearAllInlineStyles() const { return !mHTMLProperty; }

  explicit EditorInlineStyle(nsStaticAtom& aHTMLProperty,
                             nsAtom* aAttribute = nullptr)
      : mHTMLProperty(&aHTMLProperty), mAttribute(aAttribute) {}
  EditorInlineStyle(nsStaticAtom& aHTMLProperty, RefPtr<nsAtom>&& aAttribute)
      : mHTMLProperty(&aHTMLProperty), mAttribute(std::move(aAttribute)) {}

  /**
   * Returns the instance which means remove all inline styles.
   */
  static EditorInlineStyle RemoveAllStyles() { return EditorInlineStyle(); }

  bool operator==(const EditorInlineStyle& aOther) const {
    return mHTMLProperty == aOther.mHTMLProperty &&
           mAttribute == aOther.mAttribute;
  }

 private:
  EditorInlineStyle() = default;
};

/******************************************************************************
 * EditorInlineStyleAndValue represents an inline style and stores its value.
 ******************************************************************************/

struct MOZ_STACK_CLASS EditorInlineStyleAndValue : public EditorInlineStyle {
  // Stores the value of mAttribute.
  nsString const mAttributeValue;

  bool IsStyleToClearAllInlineStyles() const = delete;
  EditorInlineStyleAndValue() = delete;

  explicit EditorInlineStyleAndValue(nsStaticAtom& aHTMLProperty)
      : EditorInlineStyle(aHTMLProperty) {}
  EditorInlineStyleAndValue(nsStaticAtom& aHTMLProperty, nsAtom& aAttribute,
                            const nsAString& aValue)
      : EditorInlineStyle(aHTMLProperty, &aAttribute),
        mAttributeValue(aValue) {}
  EditorInlineStyleAndValue(nsStaticAtom& aHTMLProperty,
                            RefPtr<nsAtom>&& aAttribute,
                            const nsAString& aValue)
      : EditorInlineStyle(aHTMLProperty, std::move(aAttribute)),
        mAttributeValue(aValue) {
    MOZ_ASSERT(mAttribute);
  }
  EditorInlineStyleAndValue(nsStaticAtom& aHTMLProperty, nsAtom& aAttribute,
                            nsString&& aValue)
      : EditorInlineStyle(aHTMLProperty, &aAttribute),
        mAttributeValue(std::move(aValue)) {}
  EditorInlineStyleAndValue(nsStaticAtom& aHTMLProperty,
                            RefPtr<nsAtom>&& aAttribute, nsString&& aValue)
      : EditorInlineStyle(aHTMLProperty, std::move(aAttribute)),
        mAttributeValue(aValue) {}

  // mHTMLProperty is never nullptr since all constructors guarantee it.
  // Therefore, hide it and expose its reference instead.
  MOZ_KNOWN_LIVE nsStaticAtom& HTMLPropertyRef() const {
    MOZ_DIAGNOSTIC_ASSERT(mHTMLProperty);
    return *mHTMLProperty;
  }

 private:
  using EditorInlineStyle::mHTMLProperty;
};

}  // namespace mozilla

#endif  // #ifndef mozilla_HTMLEditHelpers_h

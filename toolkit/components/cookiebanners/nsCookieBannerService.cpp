/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsCookieBannerService.h"

#include "nsCookieBannerRule.h"
#include "nsCookieInjector.h"
#include "nsIClickRule.h"
#include "nsICookieBannerListService.h"
#include "nsICookieBannerRule.h"
#include "nsICookie.h"
#include "nsIEffectiveTLDService.h"
#include "mozilla/StaticPrefs_cookiebanners.h"
#include "ErrorList.h"
#include "mozilla/Logging.h"
#include "nsDebug.h"
#include "nsCOMPtr.h"
#include "nsNetCID.h"
#include "nsServiceManagerUtils.h"
#include "nsCRT.h"
#include "mozilla/ClearOnShutdown.h"

namespace mozilla {

NS_IMPL_ISUPPORTS(nsCookieBannerService, nsICookieBannerService, nsIObserver)

LazyLogModule gCookieBannerLog("nsCookieBannerService");

static const char kCookieBannerServiceModePref[] = "cookiebanners.service.mode";

static StaticRefPtr<nsCookieBannerService> sCookieBannerServiceSingleton;

// static
already_AddRefed<nsCookieBannerService> nsCookieBannerService::GetSingleton() {
  if (!sCookieBannerServiceSingleton) {
    sCookieBannerServiceSingleton = new nsCookieBannerService();

    RunOnShutdown([] {
      MOZ_LOG(gCookieBannerLog, LogLevel::Debug,
              ("RunOnShutdown. Mode: %d",
               StaticPrefs::cookiebanners_service_mode()));

      // Unregister pref listeners.
      DebugOnly<nsresult> rv = Preferences::UnregisterCallback(
          &nsCookieBannerService::OnPrefChange, kCookieBannerServiceModePref);
      NS_WARNING_ASSERTION(
          NS_SUCCEEDED(rv),
          "Unregistering kCookieBannerServiceModePref callback failed");

      rv = sCookieBannerServiceSingleton->Shutdown();
      NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                           "nsCookieBannerService::Shutdown failed.");

      sCookieBannerServiceSingleton = nullptr;
    });
  }

  return do_AddRef(sCookieBannerServiceSingleton);
}

// static
void nsCookieBannerService::OnPrefChange(const char* aPref, void* aData) {
  RefPtr<nsCookieBannerService> service = GetSingleton();

  if (StaticPrefs::cookiebanners_service_mode() !=
      nsICookieBannerService::MODE_DISABLED) {
    MOZ_LOG(
        gCookieBannerLog, LogLevel::Info,
        ("Initializing nsCookieBannerService after pref change. %s", aPref));
    DebugOnly<nsresult> rv = service->Init();
    NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                         "nsCookieBannerService::Init failed");
    return;
  }

  MOZ_LOG(gCookieBannerLog, LogLevel::Info,
          ("Disabling nsCookieBannerService after pref change. %s", aPref));

  DebugOnly<nsresult> rv = service->Shutdown();
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "nsCookieBannerService::Shutdown failed");
}

// This method initializes the cookie banner service on startup on
// "profile-after-change".
NS_IMETHODIMP
nsCookieBannerService::Observe(nsISupports* aSubject, const char* aTopic,
                               const char16_t* aData) {
  if (nsCRT::strcmp(aTopic, "profile-after-change") != 0) {
    return NS_OK;
  }

  return Preferences::RegisterCallbackAndCall(
      &nsCookieBannerService::OnPrefChange, kCookieBannerServiceModePref);
}

nsresult nsCookieBannerService::Init() {
  MOZ_LOG(gCookieBannerLog, LogLevel::Debug,
          ("%s. Mode: %d", __FUNCTION__,
           StaticPrefs::cookiebanners_service_mode()));

  // Check if already initialized.
  if (mIsInitialized) {
    return NS_OK;
  }

  // Initialize the service which fetches cookie banner rules.
  mListService = do_GetService(NS_COOKIEBANNERLISTSERVICE_CONTRACTID);
  NS_ENSURE_TRUE(mListService, NS_ERROR_FAILURE);

  // Setting mIsInitialized before importing rules, because the list service
  // needs to call nsCookieBannerService methods that would throw if not marked
  // initialized.
  mIsInitialized = true;

  // Import initial rule-set and enable rule syncing.
  mListService->Init();

  // Initialize the cookie injector.
  RefPtr<nsCookieInjector> injector = nsCookieInjector::GetSingleton();

  return NS_OK;
}

nsresult nsCookieBannerService::Shutdown() {
  MOZ_LOG(gCookieBannerLog, LogLevel::Debug,
          ("%s. Mode: %d", __FUNCTION__,
           StaticPrefs::cookiebanners_service_mode()));

  // Check if already shutdown.
  if (!mIsInitialized) {
    return NS_OK;
  }
  mIsInitialized = false;

  // Shut down the list service which will stop updating mRules.
  mListService->Shutdown();

  // Clear all stored cookie banner rules. They will be imported again on Init.
  mRules.Clear();

  return NS_OK;
}

NS_IMETHODIMP
nsCookieBannerService::GetRules(nsTArray<RefPtr<nsICookieBannerRule>>& aRules) {
  aRules.Clear();

  // Service is disabled, throw with empty array.
  if (!mIsInitialized) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  AppendToArray(aRules, mRules.Values());

  return NS_OK;
}

NS_IMETHODIMP
nsCookieBannerService::ResetRules(const bool doImport) {
  // Service is disabled, throw.
  if (!mIsInitialized) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  mRules.Clear();

  if (doImport) {
    NS_ENSURE_TRUE(mListService, NS_ERROR_FAILURE);
    nsresult rv = mListService->ImportAllRules();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

nsresult nsCookieBannerService::GetRuleForDomain(const nsACString& aDomain,
                                                 nsICookieBannerRule** aRule) {
  NS_ENSURE_ARG_POINTER(aRule);
  *aRule = nullptr;

  // Service is disabled, throw with null.
  if (!mIsInitialized) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsCOMPtr<nsICookieBannerRule> rule = mRules.Get(aDomain);
  if (rule) {
    rule.forget(aRule);
  }

  return NS_OK;
}

nsresult nsCookieBannerService::GetRuleForURI(nsIURI* aURI,
                                              nsICookieBannerRule** aRule) {
  NS_ENSURE_ARG_POINTER(aURI);
  NS_ENSURE_ARG_POINTER(aRule);
  *aRule = nullptr;

  // Service is disabled, throw with null.
  if (!mIsInitialized) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsresult rv;
  nsCOMPtr<nsIEffectiveTLDService> eTLDService(
      do_GetService(NS_EFFECTIVETLDSERVICE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCString baseDomain;
  rv = eTLDService->GetBaseDomain(aURI, 0, baseDomain);
  NS_ENSURE_SUCCESS(rv, rv);

  return GetRuleForDomain(baseDomain, aRule);
}

NS_IMETHODIMP
nsCookieBannerService::GetCookiesForURI(
    nsIURI* aURI, nsTArray<RefPtr<nsICookieRule>>& aCookies) {
  aCookies.Clear();

  // Service is disabled, throw with empty array.
  if (!mIsInitialized) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsCOMPtr<nsICookieBannerRule> rule;
  nsresult rv = GetRuleForURI(aURI, getter_AddRefs(rule));
  NS_ENSURE_SUCCESS(rv, rv);

  if (!rule) {
    return NS_OK;
  }

  // MODE_REJECT: In this mode we only handle the banner if we can reject. We
  // don't care about the opt-in cookies.
  rv = rule->GetCookiesOptOut(aCookies);
  NS_ENSURE_SUCCESS(rv, rv);

  // MODE_REJECT_OR_ACCEPT: In this mode we will try to opt-out, but if we don't
  // have any opt-out cookies we will fallback to the opt-in cookies.
  if (StaticPrefs::cookiebanners_service_mode() ==
          nsICookieBannerService::MODE_REJECT_OR_ACCEPT &&
      aCookies.IsEmpty()) {
    return rule->GetCookiesOptIn(aCookies);
  }

  return NS_OK;
}

NS_IMETHODIMP
nsCookieBannerService::GetClickRuleForDomain(const nsACString& aDomain,
                                             nsIClickRule** aRule) {
  NS_ENSURE_ARG_POINTER(aRule);

  // Service is disabled, throw with empty rule.
  if (!mIsInitialized) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsCOMPtr<nsICookieBannerRule> rule;
  nsresult rv = GetRuleForDomain(aDomain, getter_AddRefs(rule));
  NS_ENSURE_SUCCESS(rv, rv);

  // No click rule for the given domain, bail out.
  if (!rule) {
    return NS_OK;
  }

  nsCOMPtr<nsIClickRule> clickRule;
  rv = rule->GetClickRule(getter_AddRefs(clickRule));
  NS_ENSURE_SUCCESS(rv, rv);

  clickRule.forget(aRule);
  return NS_OK;
}

NS_IMETHODIMP
nsCookieBannerService::InsertRule(nsICookieBannerRule* aRule) {
  NS_ENSURE_ARG_POINTER(aRule);

  // Service is disabled, throw.
  if (!mIsInitialized) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsAutoCString domain;
  nsresult rv = aRule->GetDomain(domain);
  NS_ENSURE_SUCCESS(rv, rv);
  NS_ENSURE_TRUE(!domain.IsEmpty(), NS_ERROR_FAILURE);

  MOZ_LOG(gCookieBannerLog, LogLevel::Debug,
          ("%s. domain: %s", __FUNCTION__, domain.get()));

  nsCOMPtr<nsICookieBannerRule> result = mRules.InsertOrUpdate(domain, aRule);
  NS_ENSURE_TRUE(result, NS_ERROR_FAILURE);

  return NS_OK;
}

NS_IMETHODIMP
nsCookieBannerService::RemoveRuleForDomain(const nsACString& aDomain) {
  NS_ENSURE_TRUE(!aDomain.IsEmpty(), NS_ERROR_FAILURE);

  // Service is disabled, throw.
  if (!mIsInitialized) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  MOZ_LOG(gCookieBannerLog, LogLevel::Debug,
          ("%s. aDomain: %s", __FUNCTION__, PromiseFlatCString(aDomain).get()));

  mRules.Remove(aDomain);

  return NS_OK;
}

}  // namespace mozilla

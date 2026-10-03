# Firefox, installed the way a user gets it: the App Store's "Install"
# button unpacks Mozilla's full installer (the download its catalog lists,
# put in C:\Downloads) with 7-Zip into C:\Programs\Mozilla Firefox.  It
# then loads a page from the HTTPS server tools/appcorpus.py runs on the
# host, with a certificate from a test CA made for the run; the screenshot
# of the page must match tests/reference/firefox.png.  A policies.json in
# Firefox's distribution folder (Mozilla's documented way to configure a
# deployment) trusts that CA and turns off the first-run pages, the
# terms-of-use prompt, update checks and telemetry.  Windowed; Firefox's
# launcher process exits once the browser process it starts is up.
import json, os, shutil

DOC = 'Firefox'
CORE = r'C:\Programs\Mozilla Firefox\core'


def settings(app, files, programs):
    """distribution\\policies.json and the test CA beside firefox.exe"""
    d = os.path.join(programs, 'Mozilla Firefox', 'core', 'distribution')
    os.makedirs(d, exist_ok=True)
    shutil.copy(app.ca, os.path.join(d, 'novaos-test-ca.pem'))
    policies = {'policies': {
        'Certificates': {'Install': [CORE + r'\distribution\novaos-test-ca.pem']},
        'DisableAppUpdate': True, 'DisableTelemetry': True, 'DisableDefaultBrowserAgent': True,
        'DontCheckDefaultBrowser': True, 'NoDefaultBookmarks': True, 'SkipTermsOfUse': True,
        'OverrideFirstRunPage': '', 'OverridePostUpdatePage': '',
        'UserMessaging': {'WhatsNew': False, 'ExtensionRecommendations': False, 'FeatureRecommendations': False,
                          'SkipOnboarding': True, 'MoreFromMozilla': False}}}
    with open(os.path.join(d, 'policies.json'), 'w') as f:
        json.dump(policies, f, indent=2)


APP = App('Firefox', '157.0',
          'https://archive.mozilla.org/pub/firefox/releases/157.0/win64/en-US/Firefox%20Setup%20157.0.exe',
          'Firefox', [Test('install from the App Store', 'store install Firefox', store='Firefox', timeout=1500),
                      Test('load an HTTPS page', rf'start "{CORE}\firefox.exe" https://10.0.2.2:{HTTPS_PORT}/',
                           timeout=360)],
          unpack=settings, gui=True, https=True, store='Firefox', processes=True)

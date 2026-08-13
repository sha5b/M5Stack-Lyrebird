export const SITE_URL = 'https://lyrebird.variable.gallery';
export const SITE_NAME = 'Lyrebird';
export const SITE_TITLE = 'Lyrebird — Flash a Bird Chorus onto an M5Stack Fire';
export const SITE_DESCRIPTION =
    'A birdsong synthesizer for the M5Stack Fire. Twelve songbird species rendered in ' +
    'real time by a physical model of the avian syrinx — no samples, no SD card. It ' +
    'boots into all twelve at once and steps down to one species at a time. ' +
    'Plug the device in and flash it from the browser.';

export const SITE_KEYWORDS =
    'lyrebird, birdsong, syrinx, birdsong synthesis, m5stack, m5stack fire, ESP32, ' +
    'web serial, esp web tools, web installer, firmware flasher, generative audio, ' +
    'physical modeling, bioacoustics, creative coding, PlatformIO, ' +
    'Shahab Nedaei, variable.gallery';

export const ARTIST = {
    name: 'Shahab Nedaei',
    url: 'https://shahabnedaei.variable.gallery',
    sameAs: ['https://github.com/sha5b']
};

// The browser instrument this firmware is a port of. The link goes to the live site
// rather than the repository — that is where the work is, and this page borrows its
// ground (see app.css). REPO_URL below is this project's own source.
export const PARENT_PROJECT = {
    name: 'Lyrebird for the browser',
    url: 'https://lyrebird.variable.gallery'
};

export const REPO_URL = 'https://github.com/sha5b/M5Stack-Lyrebird';

export const OG_IMAGE = `${SITE_URL}/og.jpg`;
export const OG_IMAGE_ALT =
    'An M5Stack Fire on a desk, its screen reading LYREBIRD with the name of a songbird species.';

export function jsonLd(): string {
    return JSON.stringify({
        '@context': 'https://schema.org',
        '@graph': [
            {
                '@type': 'WebSite',
                '@id': `${SITE_URL}/#website`,
                url: SITE_URL,
                name: SITE_NAME,
                inLanguage: 'en'
            },
            {
                '@type': 'WebApplication',
                '@id': `${SITE_URL}/#app`,
                url: SITE_URL,
                name: SITE_NAME,
                description: SITE_DESCRIPTION,
                applicationCategory: 'DeveloperApplication',
                operatingSystem: 'Any browser with Web Serial (Chrome, Edge, Opera)',
                offers: { '@type': 'Offer', price: '0', priceCurrency: 'EUR' },
                isPartOf: { '@id': `${SITE_URL}/#website` },
                author: {
                    '@type': 'Person',
                    name: ARTIST.name,
                    url: ARTIST.url,
                    sameAs: ARTIST.sameAs
                },
                isBasedOn: PARENT_PROJECT.url,
                codeRepository: REPO_URL
            }
        ]
    });
}

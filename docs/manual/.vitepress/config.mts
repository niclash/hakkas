import {defineConfig} from 'vitepress';

// The path the site is served under: HAKKAS_MANUAL_BASE when building, else /hakkas/user-manual/
// (decided with the user 2026-10-27); always with a leading and a trailing slash.
const base = ('/' + (process.env.HAKKAS_MANUAL_BASE ?? '/hakkas/user-manual/') + '/').replace(/\/+/g, '/');

// The hakkas User's Manual (plan Section U): the outline decided with the user on 2026-10-27,
// one page per chapter, grouped by part.
export default defineConfig({
    title: "Hakkas User's Manual",
    description: 'Building flow-based control programs with hakkas',
    lang: 'en',
    base,
    cleanUrls: true,
    lastUpdated: false,
    themeConfig: {
        search: {provider: 'local'},
        outline: {level: [2, 3], label: 'In this chapter'},
        docFooter: {prev: 'Previous', next: 'Next'},
        sidebar: [
            {text: 'Part I — Getting started', items: [
                {text: '1. Introduction', link: '/getting-started/introduction'},
                {text: '2. Concepts', link: '/getting-started/concepts'},
                {text: '3. First contact', link: '/getting-started/first-contact'},
            ]},
            {text: 'Part II — Example 1: a thermostat', items: [
                {text: '4. Building the thermostat', link: '/thermostat/'},
            ]},
            {text: 'Part III — Working with hakkas', items: [
                {text: '5. The process editor', link: '/working/editor'},
                {text: '6. Processes', link: '/working/processes'},
                {text: '7. Composite block types', link: '/working/composite-types'},
                {text: '8. The environment', link: '/working/environment'},
                {text: '9. Moving work between systems', link: '/working/moving-work'},
                {text: '10. Users, roles and sessions', link: '/working/users'},
                {text: '11. Links between processes', link: '/working/links'},
            ]},
            {text: 'Part IV — Example 2: a heating circuit', items: [
                {text: '12. The heating circuit', link: '/intermediate/'},
            ]},
            {text: 'Part V — Example 3: an air handling unit', items: [
                {text: '13. The air handling unit', link: '/ahu/'},
            ]},
            {text: 'Part VI — Patterns and reference', items: [
                {text: '14. Design patterns', link: '/reference/patterns'},
                {text: '15. Block type reference', link: '/reference/block-types'},
                {text: '16. Administration', link: '/reference/administration'},
            ]},
            {text: 'Appendices', items: [
                {text: 'A. IEC 61131-3 and hakkas', link: '/appendix/iec-61131-3'},
                {text: 'B. Glossary', link: '/appendix/glossary'},
                {text: 'C. Troubleshooting', link: '/appendix/troubleshooting'},
                {text: 'D. The Colibri emulator', link: '/appendix/emulator'},
            ]},
        ],
    },
});

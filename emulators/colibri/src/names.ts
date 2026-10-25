// An environment's name (control protocol §4.4): 1-64 characters of letters, digits, `.`, `_`
// and `-`, not starting with `.`. It is a file name and hakkas' environment name (04 §0).
const ENVIRONMENT_NAME = /^[A-Za-z0-9_-][A-Za-z0-9._-]{0,63}$/;

export function isEnvironmentName(name: string): boolean {
    return ENVIRONMENT_NAME.test(name);
}

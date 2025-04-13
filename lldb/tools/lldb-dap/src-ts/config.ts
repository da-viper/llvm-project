import * as vscode from "vscode";
import * as path from "path";
import * as os from "os";

/**
 * \$                   match dollar sign.
 * {                    match open curly brackets.
 * (?:                  don't capture group.
 * (env|command))       capture if it starts with env or command.
 * :                    match semicolon.
 * )?                   close and match group zero or one time.
 * (.*?)                match any character zero or more times lazily.
 * }                    match close curly brackets.
 */
const envRegex = /\${(?:(env|command):)?(.*?)}/g;


function replaceEnvironment(prefix: string | undefined, envValue: string): string | undefined {
    if (!prefix) {
        // replace a subset of vscode predefined variables 
        // that makes sense in settings.json.
        switch (envValue) {
            case "userHome":
                return os.homedir();
            case "workspaceFolder":
                const workspaceFolder = vscode.workspace.workspaceFolders
                if (workspaceFolder && workspaceFolder.length > 0) {
                    return workspaceFolder[0].uri.fsPath;
                }
                break;
            case "pathSeparator":
            case "/":
                return path.sep;
            default:
                // if no prefix interpret as normal environment variables.
                return process.env[envValue] ?? "";
        }
    }

    switch (prefix) {
        case "env":
            return process.env[envValue] ?? "";
        case "config":
            return vscode.workspace.getConfiguration().get<string>(envValue);
        default:
    }

    return undefined;
}

export function substituteEnv(input: string): string {
    let regex = envRegex;

    const result = input.replace(regex, (match, prefix, value) => {
        let replacedValue = replaceEnvironment(prefix, value);

        // check for undefined as environment variables may be empty
        if (replacedValue !== undefined) {
            return replacedValue;
        }
        return match;
    });

    return result;
}
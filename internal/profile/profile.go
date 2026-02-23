package profile

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/librescoot/dbc-dispatcher/internal/config"
)

type Profile struct {
	Binary string
	Args   []string
	Env    []string
	Dir    string
}

const flutterGlob = "/usr/share/flutter/scootui/*/release"

func discoverFlutterPath() (string, error) {
	matches, err := filepath.Glob(flutterGlob)
	if err != nil {
		return "", fmt.Errorf("glob %s: %w", flutterGlob, err)
	}
	if len(matches) == 0 {
		return "", fmt.Errorf("no flutter release directory found matching %s", flutterGlob)
	}
	return matches[0], nil
}

func builtinScootUI() (*Profile, error) {
	bundlePath, err := discoverFlutterPath()
	if err != nil {
		return nil, err
	}

	return &Profile{
		Binary: "/usr/bin/flutter-drm-gbm-backend",
		Args:   []string{"flutter-drm-gbm-backend", "-b", bundlePath},
		Dir:    bundlePath,
	}, nil
}

var builtins = map[string]func() (*Profile, error){
	"scootui": builtinScootUI,
}

// Resolve resolves an app name to a Profile. It checks, in order:
// 1. Custom profiles from the config file
// 2. Built-in profiles (e.g. "scootui")
// 3. Absolute path (treated as a direct binary)
func Resolve(appName string, fileCfg *config.FileConfig) (*Profile, error) {
	if fileCfg != nil {
		if app, ok := fileCfg.Apps[appName]; ok {
			return &Profile{
				Binary: app.Binary,
				Args:   app.Args,
				Env:    app.Env,
				Dir:    app.Dir,
			}, nil
		}
	}

	if fn, ok := builtins[appName]; ok {
		return fn()
	}

	if filepath.IsAbs(appName) {
		if _, err := os.Stat(appName); err != nil {
			return nil, fmt.Errorf("binary not found: %s", appName)
		}
		return &Profile{
			Binary: appName,
			Args:   []string{filepath.Base(appName)},
		}, nil
	}

	return nil, fmt.Errorf("unknown app %q: not a built-in profile and not an absolute path", appName)
}

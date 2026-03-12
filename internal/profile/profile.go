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

func discoverFlutterBackend() (string, string) {
	if _, err := os.Stat("/usr/bin/flutter-fbdev-backend"); err == nil {
		return "/usr/bin/flutter-fbdev-backend", "flutter-fbdev-backend"
	}
	return "/usr/bin/flutter-drm-gbm-backend", "flutter-drm-gbm-backend"
}

func builtinScootUI() (*Profile, error) {
	bundlePath, err := discoverFlutterPath()
	if err != nil {
		return nil, err
	}

	binary, argv0 := discoverFlutterBackend()
	return &Profile{
		Binary: binary,
		Args:   []string{argv0, "-b", bundlePath},
		Dir:    bundlePath,
	}, nil
}

func builtinScootUIQt() (*Profile, error) {
	const binary = "/usr/bin/scootui-qt"
	if _, err := os.Stat(binary); err != nil {
		return nil, fmt.Errorf("scootui-qt binary not found: %w", err)
	}
	return &Profile{
		Binary: binary,
		Args:   []string{"scootui-qt"},
		Env: []string{
			"QT_QPA_PLATFORM=eglfs",
			"QT_QPA_EGLFS_INTEGRATION=eglfs_kms",
			"QT_QPA_EGLFS_KMS_CONFIG=/etc/scootui-qt-kms.json",
			"QT_PLUGIN_PATH=/usr/plugins:/usr/lib/plugins",
		},
	}, nil
}

var builtins = map[string]func() (*Profile, error){
	"scootui":    builtinScootUI,
	"scootui-qt": builtinScootUIQt,
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

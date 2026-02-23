package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"syscall"
	"time"

	"github.com/redis/go-redis/v9"

	"github.com/librescoot/dbc-dispatcher/internal/config"
	"github.com/librescoot/dbc-dispatcher/internal/profile"
)

var version = "dev"

const (
	defaultApp      = "scootui"
	redisKey        = "settings"
	redisField      = "dashboard.app"
	retryInterval   = 500 * time.Millisecond
)

func main() {
	showVersion := flag.Bool("version", false, "Print version and exit")
	redisURL := flag.String("redis-url", "redis://192.168.7.1:6379", "Redis URL")
	configFile := flag.String("config", "/etc/dbc-dispatcher.conf", "Config file path")
	timeout := flag.Duration("timeout", 5*time.Second, "Redis connection timeout")
	flag.Parse()

	if *showVersion {
		fmt.Printf("dbc-dispatcher %s\n", version)
		return
	}

	if os.Getenv("JOURNAL_STREAM") != "" {
		log.SetFlags(0)
	} else {
		log.SetFlags(log.Ldate | log.Ltime | log.Lmicroseconds)
	}

	log.Printf("dbc-dispatcher %s starting", version)

	// Load optional config file
	var fileCfg *config.FileConfig
	if _, err := os.Stat(*configFile); err == nil {
		fileCfg, err = config.LoadFile(*configFile)
		if err != nil {
			log.Printf("warning: failed to parse config %s: %v", *configFile, err)
		} else {
			log.Printf("loaded config from %s", *configFile)
		}
	}

	appName := readSetting(*redisURL, *timeout)
	log.Printf("app=%q", appName)

	p, err := profile.Resolve(appName, fileCfg)
	if err != nil {
		log.Fatalf("failed to resolve app %q: %v", appName, err)
	}

	// Merge environment: inherit current env, append profile-specific vars
	env := os.Environ()
	env = append(env, p.Env...)

	if p.Dir != "" {
		if err := os.Chdir(p.Dir); err != nil {
			log.Printf("warning: chdir %s: %v", p.Dir, err)
		}
	}

	log.Printf("exec %s %v", p.Binary, p.Args)
	err = syscall.Exec(p.Binary, p.Args, env)
	log.Fatalf("exec failed: %v", err)
}

func readSetting(redisURL string, timeout time.Duration) string {
	opts, err := redis.ParseURL(redisURL)
	if err != nil {
		log.Printf("invalid redis URL %q: %v, using default", redisURL, err)
		return defaultApp
	}

	deadline := time.Now().Add(timeout)
	for {
		ctx, cancel := context.WithTimeout(context.Background(), retryInterval)
		client := redis.NewClient(opts)

		val, err := client.HGet(ctx, redisKey, redisField).Result()
		client.Close()
		cancel()

		if err == nil && val != "" {
			return val
		}

		if err != nil && err != redis.Nil {
			if time.Now().After(deadline) {
				log.Printf("redis unreachable after %v, using default", timeout)
				return defaultApp
			}
			time.Sleep(retryInterval)
			continue
		}

		// redis.Nil means key/field not set
		log.Printf("setting %s not set, using default", redisField)
		return defaultApp
	}
}

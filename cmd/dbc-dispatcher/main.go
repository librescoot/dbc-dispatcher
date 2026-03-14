package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/coreos/go-systemd/v22/dbus"
	"github.com/redis/go-redis/v9"
)

var version = "dev"

const (
	defaultApp    = "scootui"
	redisKey      = "settings"
	redisField    = "dashboard.app"
	retryInterval = 500 * time.Millisecond
	unitSuffix    = ".service"
)

func main() {
	showVersion := flag.Bool("version", false, "Print version and exit")
	redisURL := flag.String("redis-url", "redis://192.168.7.1:6379", "Redis URL")
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

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer cancel()

	conn, err := dbus.NewSystemConnectionContext(ctx)
	if err != nil {
		log.Fatalf("failed to connect to systemd D-Bus: %v", err)
	}
	defer conn.Close()

	rdb := connectRedis(ctx, *redisURL, *timeout)
	defer rdb.Close()

	appName := readSetting(ctx, rdb)
	log.Printf("app=%q", appName)

	currentUnit := unitName(appName)
	if err := startUnit(ctx, conn, currentUnit); err != nil {
		log.Fatalf("failed to start %s: %v", currentUnit, err)
	}

	// Subscribe to settings changes and watch for app switches
	pubsub := rdb.Subscribe(ctx, redisField)
	defer pubsub.Close()

	log.Printf("watching for %s changes", redisField)

	for {
		select {
		case <-ctx.Done():
			log.Printf("shutting down, stopping %s", currentUnit)
			stopCtx, stopCancel := context.WithTimeout(context.Background(), 5*time.Second)
			stopUnit(stopCtx, conn, currentUnit)
			stopCancel()
			return

		case msg := <-pubsub.Channel():
			newApp := strings.TrimSpace(msg.Payload)
			if newApp == "" {
				newApp = defaultApp
			}
			newUnit := unitName(newApp)
			if newUnit == currentUnit {
				continue
			}

			log.Printf("switching %s -> %s", currentUnit, newUnit)

			stopCtx, stopCancel := context.WithTimeout(ctx, 10*time.Second)
			stopUnit(stopCtx, conn, currentUnit)
			stopCancel()

			if err := startUnit(ctx, conn, newUnit); err != nil {
				log.Printf("failed to start %s: %v, reverting to %s", newUnit, err, currentUnit)
				if err := startUnit(ctx, conn, currentUnit); err != nil {
					log.Printf("revert also failed: %v", err)
				}
				continue
			}

			currentUnit = newUnit
		}
	}
}

func unitName(app string) string {
	if strings.HasSuffix(app, unitSuffix) {
		return app
	}
	return app + unitSuffix
}

func startUnit(ctx context.Context, conn *dbus.Conn, unit string) error {
	log.Printf("starting %s", unit)
	ch := make(chan string, 1)
	_, err := conn.StartUnitContext(ctx, unit, "replace", ch)
	if err != nil {
		return fmt.Errorf("start %s: %w", unit, err)
	}

	select {
	case result := <-ch:
		if result != "done" {
			return fmt.Errorf("start %s: job result %q", unit, result)
		}
		log.Printf("started %s", unit)
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

func stopUnit(ctx context.Context, conn *dbus.Conn, unit string) {
	log.Printf("stopping %s", unit)
	ch := make(chan string, 1)
	_, err := conn.StopUnitContext(ctx, unit, "replace", ch)
	if err != nil {
		log.Printf("stop %s: %v", unit, err)
		return
	}

	select {
	case result := <-ch:
		if result != "done" {
			log.Printf("stop %s: job result %q", unit, result)
		} else {
			log.Printf("stopped %s", unit)
		}
	case <-ctx.Done():
		log.Printf("stop %s: timed out", unit)
	}
}

func connectRedis(ctx context.Context, redisURL string, timeout time.Duration) *redis.Client {
	opts, err := redis.ParseURL(redisURL)
	if err != nil {
		log.Fatalf("invalid redis URL %q: %v", redisURL, err)
	}

	client := redis.NewClient(opts)

	deadline := time.Now().Add(timeout)
	for {
		err := client.Ping(ctx).Err()
		if err == nil {
			return client
		}
		if time.Now().After(deadline) {
			log.Printf("redis unreachable after %v, continuing anyway", timeout)
			return client
		}
		time.Sleep(retryInterval)
	}
}

func readSetting(ctx context.Context, rdb *redis.Client) string {
	val, err := rdb.HGet(ctx, redisKey, redisField).Result()
	if err != nil || val == "" {
		if err != nil && err != redis.Nil {
			log.Printf("redis read error: %v, using default", err)
		}
		return defaultApp
	}
	return val
}

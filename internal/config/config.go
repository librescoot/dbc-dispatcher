package config

import (
	"os"

	"github.com/BurntSushi/toml"
)

type AppProfile struct {
	Binary string   `toml:"binary"`
	Args   []string `toml:"args"`
	Env    []string `toml:"env"`
	Dir    string   `toml:"dir"`
}

type FileConfig struct {
	Apps map[string]AppProfile `toml:"apps"`
}

func LoadFile(path string) (*FileConfig, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	var cfg FileConfig
	if err := toml.Unmarshal(data, &cfg); err != nil {
		return nil, err
	}

	return &cfg, nil
}

package jlog

import (
	"bufio"
	"encoding/hex"
	"encoding/json"
	"log"
	"os"
	"regexp"

	"github.com/xen0bit/rattagatt/rg-loader/pkg/dbtools"
)

type LogLine struct {
	SrcMac string         `json:"mac"`
	Logs   map[string]Log `json:"logs"`
}

type Log struct {
	Name        string `json:"name"`
	Rssi        int    `json:"rssi"`
	Man         string `json:"man"`
	Connectable bool   `json:"connectable"`
	AddrType    int    `json:"addr_type"`
	Tree        []struct {
		Svc  string `json:"svc"`
		Chr  string `json:"chr"`
		Val  string `json:"val"`
		Prop int    `json:"prop"`
	} `json:"tree"`
}

type Advertisment struct {
	Mac              string
	Name             string
	ManufacturerData []byte
}

type Characteristic struct {
	Mac                string
	ServiceUUID        string
	CharacteristicUUID string
	ReadVal            []byte
}

type Service struct {
	Mac         string
	ServiceUUID string
}

func isMacString(mac string) bool {
	m, err := regexp.Match("^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$", []byte(mac))
	if err != nil {
		panic(err)
	}
	return m
}

func ReadLog(path string, logch chan dbtools.RecordRow) {
	file, err := os.Open(path)
	if err != nil {
		log.Fatal(err)
	}
	defer file.Close()

	scanner := bufio.NewScanner(file)
	ecnt := 0
	gid := 0
	for scanner.Scan() {
		var ll LogLine
		err := json.Unmarshal(scanner.Bytes(), &ll)
		if err == nil {
			for mac, log := range ll.Logs {
				if isMacString(mac) {
					var record dbtools.RecordRow
					record.Gid = gid
					//Mac
					record.Mac = mac
					//Name
					name, err := hex.DecodeString(log.Name)
					if err == nil {
						record.Name = string(name)
					}
					//Manufacturer Data
					man, err := hex.DecodeString(log.Man)
					if err == nil {
						record.ManufacturerData = man
					}

					//Just Advertisment data
					//fmt.Println(record)
					logch <- record

					//Check if we got the device tree
					if len(log.Tree) > 0 {
						for _, branch := range log.Tree {
							//Service
							record.ServiceUUID = branch.Svc
							//Characteristic
							record.CharacteristicUUID = branch.Chr
							//Properties
							record.Properties = branch.Prop
							//ReadValue
							rv, err := hex.DecodeString(branch.Val)
							if err == nil {
								record.ReadValue = rv
							}
							//fmt.Println(record)
							logch <- record
						}
					}
				}
			}
		} else {
			ecnt++
		}
		gid++
	}
	log.Println("Error Count:", ecnt)
	close(logch)

	if err := scanner.Err(); err != nil {
		log.Fatal(err)
	}
}

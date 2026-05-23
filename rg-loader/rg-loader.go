package main

import (
	"bufio"
	"flag"
	"fmt"
	"log"
	"os"

	"github.com/xen0bit/rattagatt/rg-loader/pkg/csvloader"
	"github.com/xen0bit/rattagatt/rg-loader/pkg/dbtools"
	"github.com/xen0bit/rattagatt/rg-loader/pkg/jlog"
)

func main() {
	log.SetFlags(log.LstdFlags | log.Lshortfile)
	inLog    := flag.String("i", "", "Input log.jsonl path")
	outDb    := flag.String("o", "", "Output SQLite database path")
	ouiCSV   := flag.String("oui",  "metadata_generator/oui.csv",  "OUI lookup CSV")
	gattCSV  := flag.String("gatt", "metadata_generator/gattchars.csv", "GATT characteristics CSV")
	flag.Parse()

	if *inLog == "" || *outDb == "" {
		fmt.Fprintln(os.Stderr, "Usage: rg-loader -i log.jsonl -o out.db")
		flag.Usage()
		os.Exit(1)
	}

	conn := dbtools.NewSqliteConn(*outDb)
	defer conn.DB.Close()
	if err := conn.CreateTables(); err != nil {
		log.Fatal(err)
	}

	// OUI manufacturer lookup
	ouiFile, err := os.Open(*ouiCSV)
	if err != nil {
		log.Printf("OUI CSV not found (%s), skipping: %v", *ouiCSV, err)
	} else {
		defer ouiFile.Close()
		ouiCh := make(chan dbtools.OuiRow)
		go func() {
			s := csvloader.NewScanner(bufio.NewReader(ouiFile))
			for s.Scan() {
				ouiCh <- dbtools.OuiRow{
					Oui:         s.Text("oui"),
					CompanyName: s.Text("company_name"),
					AddressOne:  s.Text("address1"),
					AddressTwo:  s.Text("address2"),
					Country:     s.Text("country"),
				}
			}
			close(ouiCh)
		}()
		if err := conn.InsertOuiData(ouiCh); err != nil {
			log.Printf("OUI insert warning: %v", err)
		}
	}

	// GATT characteristic name lookup
	gattFile, err := os.Open(*gattCSV)
	if err != nil {
		log.Printf("GATT CSV not found (%s), skipping: %v", *gattCSV, err)
	} else {
		defer gattFile.Close()
		gattCh := make(chan dbtools.GattRow)
		go func() {
			s := csvloader.NewScanner(bufio.NewReader(gattFile))
			for s.Scan() {
				gattCh <- dbtools.GattRow{
					CharacteristicUUID: s.Text("characteristic_uuid"),
					CharacteristicName: s.Text("characteristic_name"),
				}
			}
			close(gattCh)
		}()
		if err := conn.InsertGattData(gattCh); err != nil {
			log.Printf("GATT insert warning: %v", err)
		}
	}

	// Load scan logs
	logCh := make(chan dbtools.RecordRow)
	go jlog.ReadLog(*inLog, logCh)
	if err := conn.InsertLogData(logCh); err != nil {
		log.Fatal(err)
	}

	if err := conn.GenerateRecords(); err != nil {
		log.Fatal(err)
	}

	log.Println("Done.")
}

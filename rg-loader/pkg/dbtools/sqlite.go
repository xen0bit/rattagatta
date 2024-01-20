package dbtools

import (
	"database/sql"
	"log"
	"os"

	_ "github.com/mattn/go-sqlite3"
)

type SqliteConn struct {
	err error
	DB  *sql.DB
}

type OuiRow struct {
	Oui         string
	CompanyName string
	AddressOne  string
	AddressTwo  string
	Country     string
}

type GattRow struct {
	CharacteristicUUID string
	CharacteristicName string
}

type AdvRow struct {
	Mac              string
	Name             string
	ManufacturerData []byte
}

type SvcRow struct {
	Mac         string
	ServiceUUID string
}

type CharRow struct {
	Mac                string
	ServiceUUID        string
	CharacteristicUUID string
	ReadValue          []byte
}

type RecordRow struct {
	Mac                string
	Name               string
	ManufacturerData   []byte
	ServiceUUID        string
	CharacteristicUUID string
	Properties         int
	ReadValue          []byte
}

func NewSqliteConn(dbPath string) *SqliteConn {
	os.Remove(dbPath)
	db, err := sql.Open("sqlite3", "file:"+dbPath+"?cache=shared")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA journal_mode = OFF;")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA synchronous = 0")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA cache_size = 1000000")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA locking_mode = EXCLUSIVE")
	if err != nil {
		log.Fatal(err)
	}
	_, err = db.Exec("PRAGMA temp_store = MEMORY;")
	if err != nil {
		log.Fatal(err)
	}
	//defer db.Close()
	return &SqliteConn{err, db}
}

func (sc *SqliteConn) CreateTables() error {
	statement := `
	CREATE TABLE "oui_lookup" (
		"oui"	TEXT,
		"company_name"	TEXT,
		"address1"	TEXT,
		"address2"	TEXT,
		"country"	TEXT
	);
	CREATE TABLE "char_lookup" (
		"characteristic_uuid"	TEXT,
		"characteristic_name"	TEXT
	);
	CREATE TABLE "logs" (
		id INTEGER not null primary key,
		"mac"	TEXT,
		"name"	TEXT,
		"man"	BLOB,
		"svc"	TEXT,
		"chr"	TEXT,
		"props"	INTEGER,
		"val"	BLOB
	);
	`
	_, err := sc.DB.Exec(statement)
	if err != nil {
		log.Printf("%q: %s\n", err, statement)
		return err
	} else {
		return nil
	}
}

func (sc *SqliteConn) InsertOuiData(chnl chan OuiRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO oui_lookup (oui, company_name, address1, address2, country) VALUES (?,?,?,?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Oui, v.CompanyName, v.AddressOne, v.AddressTwo, v.Country)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertGattData(chnl chan GattRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO char_lookup (characteristic_uuid,characteristic_name) VALUES (?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.CharacteristicUUID, v.CharacteristicName)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertAdvData(chnl chan AdvRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO advs (mac, name, manufacturer_data) VALUES (?,?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Mac, v.Name, v.ManufacturerData)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertSvcData(chnl chan SvcRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO svcs (mac, service_uuid) VALUES (?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Mac, v.ServiceUUID)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertCharData(chnl chan CharRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO chars (mac, service_uuid, characteristic_uuid, read_val) VALUES (?,?,?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Mac, v.ServiceUUID, v.CharacteristicUUID, v.ReadValue)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) InsertLogData(chnl chan RecordRow) error {
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare("INSERT INTO logs (mac, name, man, svc, chr, props, val) VALUES (?,?,?,?,?,?,?)")
	if err != nil {
		return err
	}
	defer stmt.Close()
	for v := range chnl {
		_, err = stmt.Exec(v.Mac, v.Name, v.ManufacturerData, v.ServiceUUID, v.CharacteristicUUID, v.Properties, v.ReadValue)
		if err != nil {
			return err
		}
	}
	tx.Commit()
	return nil
}

func (sc *SqliteConn) GenerateRecords() error {
	logDedupe := `
	CREATE TABLE IF NOT EXISTS logs_dedupe AS
	select *
	from logs
	group by mac, name, man, svc, chr, props, val;
	`

	records := `
	CREATE TABLE IF NOT EXISTS records AS
	select ld.mac                 as mac,
	name,
		ol.company_name       as company_name,
		ld.man as manufacturer_data,
		ld.svc        as service_uuid,
		ld.chr as characteristic_uuid,
		characteristic_name,
		ld.val as val
	from logs_dedupe ld
			left join char_lookup cl on ld.chr = cl.characteristic_uuid
			left join oui_lookup ol on ol.oui = SUBSTR(ld.mac, 1, 8)
	order by ld.mac, ol.company_name, ld.svc, ld.chr, cl.characteristic_name asc;
	`

	dropLogDedupe := `DROP TABLE logs_dedupe;`
	dropOui := `DROP TABLE oui_lookup;`
	dropchar := `DROP TABLE char_lookup;`

	// vac := `VACUUM;`

	//Logs dedupe
	tx, err := sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err := tx.Prepare(logDedupe)
	if err != nil {
		return err
	}
	defer stmt.Close()
	_, err = stmt.Exec()
	if err != nil {
		return err
	}
	tx.Commit()

	//Records
	tx, err = sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err = tx.Prepare(records)
	if err != nil {
		return err
	}
	defer stmt.Close()
	_, err = stmt.Exec()
	if err != nil {
		return err
	}
	tx.Commit()

	//drop
	tx, err = sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err = tx.Prepare(dropLogDedupe)
	if err != nil {
		return err
	}
	defer stmt.Close()
	_, err = stmt.Exec()
	if err != nil {
		return err
	}
	tx.Commit()
	//drop
	tx, err = sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err = tx.Prepare(dropOui)
	if err != nil {
		return err
	}
	defer stmt.Close()
	_, err = stmt.Exec()
	if err != nil {
		return err
	}
	tx.Commit()
	//drop
	tx, err = sc.DB.Begin()
	if err != nil {
		return err
	}
	stmt, err = tx.Prepare(dropchar)
	if err != nil {
		return err
	}
	defer stmt.Close()
	_, err = stmt.Exec()
	if err != nil {
		return err
	}
	tx.Commit()

	//vac
	// tx, err = sc.DB.Begin()
	// if err != nil {
	// 	return err
	// }
	// stmt, err = tx.Prepare(vac)
	// if err != nil {
	// 	return err
	// }
	// defer stmt.Close()
	// _, err = stmt.Exec()
	// if err != nil {
	// 	return err
	// }
	// tx.Commit()

	return nil
}

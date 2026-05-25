#!/usr/bin/env python3
"""
dealership-faker.py
Generates a realistic car dealership SQLite database with fake data.
"""

import sqlite3
import random
from datetime import datetime, timedelta
from faker import Faker

DB_NAME = "dealership.sqlite3"

# How many rows to generate per table
NUM_CUSTOMERS = 50_000
NUM_VEHICLES = 8_000
NUM_SALES = 25_000
NUM_PARTS = 400

fake = Faker()


def create_tables(conn):
    c = conn.cursor()

    c.execute("""
        CREATE TABLE customers (
            id INTEGER PRIMARY KEY,
            customer_number TEXT UNIQUE NOT NULL,
            first_name TEXT,
            middle_initial TEXT,
            last_name TEXT,
            address1 TEXT,
            address2 TEXT,
            city TEXT,
            state TEXT,
            zip TEXT,
            phone TEXT,
            email TEXT,
            created_at TEXT
        )
    """)

    c.execute("""
        CREATE TABLE vehicles (
            id INTEGER PRIMARY KEY,
            vin TEXT UNIQUE NOT NULL,
            year INTEGER,
            make TEXT,
            model TEXT,
            trim TEXT,
            color TEXT,
            mileage INTEGER,
            price REAL,
            status TEXT CHECK(status IN ('new', 'used', 'certified'))
        )
    """)

    c.execute("""
        CREATE TABLE sales (
            id INTEGER PRIMARY KEY,
            sale_number TEXT UNIQUE NOT NULL,
            customer_id INTEGER,
            vehicle_id INTEGER,
            sale_date TEXT,
            sale_price REAL,
            salesperson TEXT,
            payment_method TEXT,
            FOREIGN KEY(customer_id) REFERENCES customers(id),
            FOREIGN KEY(vehicle_id) REFERENCES vehicles(id)
        )
    """)

    c.execute("""
        CREATE TABLE parts (
            id INTEGER PRIMARY KEY,
            sku TEXT UNIQUE NOT NULL,
            name TEXT,
            category TEXT,
            price REAL
        )
    """)

    c.execute("""
        CREATE TABLE sale_parts (
            sale_id INTEGER,
            part_id INTEGER,
            quantity INTEGER,
            price_at_sale REAL,
            FOREIGN KEY(sale_id) REFERENCES sales(id),
            FOREIGN KEY(part_id) REFERENCES parts(id)
        )
    """)

    conn.commit()


def generate_customers(conn, count):
    c = conn.cursor()
    print(f"Generating {count:,} customers...")

    for i in range(count):
        first = fake.first_name()
        last = fake.last_name()
        mi = random.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") if random.random() > 0.3 else ""

        cust_num = f"C{100000 + i:07d}"

        c.execute("""
            INSERT INTO customers (
                customer_number, first_name, middle_initial, last_name,
                address1, address2, city, state, zip, phone, email, created_at
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            cust_num, first, mi, last,
            fake.street_address(), fake.secondary_address() if random.random() > 0.7 else "",
            fake.city(), fake.state_abbr(), fake.zipcode(),
            fake.phone_number(), fake.email(),
            fake.date_time_this_decade().isoformat()
        ))

        if i % 5000 == 0:
            conn.commit()
            print(f"  {i:,} customers inserted...")

    conn.commit()
    print("Customers done.")


def generate_vehicles(conn, count):
    c = conn.cursor()
    print(f"Generating {count:,} vehicles...")

    makes_models = {
        "Toyota": ["Camry", "Corolla", "RAV4", "Tacoma", "Highlander"],
        "Honda": ["Civic", "Accord", "CR-V", "Pilot"],
        "Ford": ["F-150", "Escape", "Explorer", "Mustang"],
        "Chevrolet": ["Silverado", "Equinox", "Malibu", "Tahoe"],
        "Nissan": ["Altima", "Rogue", "Sentra", "Pathfinder"],
        "BMW": ["3 Series", "5 Series", "X3", "X5"],
    }

    for i in range(count):
        make = random.choice(list(makes_models.keys()))
        model = random.choice(makes_models[make])
        year = random.randint(2015, 2025)
        vin = fake.unique.vin()
        color = fake.color_name()
        mileage = random.randint(0, 180_000) if year < 2023 else random.randint(0, 15000)
        price = round(random.uniform(18000, 85000), 2)
        status = random.choices(["new", "used", "certified"], weights=[0.3, 0.5, 0.2])[0]

        c.execute("""
            INSERT INTO vehicles (vin, year, make, model, trim, color, mileage, price, status)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (vin, year, make, model, fake.word().capitalize(), color, mileage, price, status))

        if i % 2000 == 0:
            conn.commit()
            print(f"  {i:,} vehicles inserted...")

    conn.commit()
    print("Vehicles done.")


def generate_sales(conn, count):
    c = conn.cursor()
    print(f"Generating {count:,} sales...")

    c.execute("SELECT id FROM customers")
    customer_ids = [row[0] for row in c.fetchall()]

    c.execute("SELECT id, price FROM vehicles")
    vehicles = c.fetchall()

    salespersons = ["John Miller", "Sarah Chen", "Mike Rodriguez", "Emily Watson", "David Park"]

    for i in range(count):
        cust_id = random.choice(customer_ids)
        vehicle = random.choice(vehicles)
        veh_id, base_price = vehicle

        sale_price = round(base_price * random.uniform(0.92, 1.05), 2)
        sale_date = (datetime.now() - timedelta(days=random.randint(0, 1200))).date().isoformat()
        sale_num = f"S{200000 + i:07d}"

        c.execute("""
            INSERT INTO sales (sale_number, customer_id, vehicle_id, sale_date, sale_price, salesperson, payment_method)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        """, (
            sale_num, cust_id, veh_id, sale_date, sale_price,
            random.choice(salespersons),
            random.choice(["Cash", "Finance", "Lease", "Credit Card"])
        ))

        if i % 5000 == 0:
            conn.commit()
            print(f"  {i:,} sales inserted...")

    conn.commit()
    print("Sales done.")


def generate_parts(conn, count):
    c = conn.cursor()
    print(f"Generating {count:,} parts...")

    categories = ["Tires", "Brakes", "Electronics", "Interior", "Exterior", "Maintenance"]

    for i in range(count):
        sku = f"P{300000 + i:06d}"
        name = fake.bs().title()
        cat = random.choice(categories)
        price = round(random.uniform(15, 850), 2)

        c.execute("INSERT INTO parts (sku, name, category, price) VALUES (?, ?, ?, ?)",
                  (sku, name, cat, price))

    conn.commit()
    print("Parts done.")


def generate_sale_parts(conn):
    c = conn.cursor()
    print("Linking parts to sales...")

    c.execute("SELECT id FROM sales")
    sale_ids = [row[0] for row in c.fetchall()]

    c.execute("SELECT id, price FROM parts")
    parts = c.fetchall()

    for sale_id in random.sample(sale_ids, k=int(len(sale_ids) * 0.65)):
        num_parts = random.randint(1, 4)
        chosen = random.sample(parts, k=min(num_parts, len(parts)))
        for part_id, price in chosen:
            qty = random.randint(1, 2)
            c.execute("""
                INSERT INTO sale_parts (sale_id, part_id, quantity, price_at_sale)
                VALUES (?, ?, ?, ?)
            """, (sale_id, part_id, qty, price))

    conn.commit()
    print("Sale parts done.")


def main():
    print("Creating dealership database...")
    conn = sqlite3.connect(DB_NAME)
    create_tables(conn)

    generate_customers(conn, NUM_CUSTOMERS)
    generate_vehicles(conn, NUM_VEHICLES)
    generate_sales(conn, NUM_SALES)
    generate_parts(conn, NUM_PARTS)
    generate_sale_parts(conn)

    conn.close()
    print(f"\nDatabase '{DB_NAME}' created successfully with realistic linked data.")


if __name__ == "__main__":
    main()

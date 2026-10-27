-- JOB (Join Order Benchmark) IMDB schema, DuckDB types.
-- The tables the benchmark queries run against; scripts/download-imdb.py loads
-- the CWI csv dump through it.
CREATE TABLE aka_name (
    id INTEGER NOT NULL PRIMARY KEY,
    person_id INTEGER NOT NULL,
    name TEXT NOT NULL,
    imdb_index VARCHAR(12),
    name_pcode_cf VARCHAR(5),
    name_pcode_nf VARCHAR(5),
    surname_pcode VARCHAR(5),
    md5sum VARCHAR(32)
);

CREATE TABLE aka_title (
    id INTEGER NOT NULL PRIMARY KEY,
    movie_id INTEGER NOT NULL,
    title TEXT NOT NULL,
    imdb_index VARCHAR(12),
    kind_id INTEGER NOT NULL,
    production_year INTEGER,
    phonetic_code VARCHAR(5),
    episode_of_id INTEGER,
    season_nr INTEGER,
    episode_nr INTEGER,
    note TEXT,
    md5sum VARCHAR(32)
);

CREATE TABLE cast_info (
    id INTEGER NOT NULL PRIMARY KEY,
    person_id INTEGER NOT NULL,
    movie_id INTEGER NOT NULL,
    person_role_id INTEGER,
    note TEXT,
    nr_order INTEGER,
    role_id INTEGER NOT NULL
);

CREATE TABLE char_name (
    id INTEGER NOT NULL PRIMARY KEY,
    name TEXT NOT NULL,
    imdb_index VARCHAR(12),
    imdb_id INTEGER,
    name_pcode_nf VARCHAR(5),
    surname_pcode VARCHAR(5),
    md5sum VARCHAR(32)
);

CREATE TABLE comp_cast_type (
    id INTEGER NOT NULL PRIMARY KEY,
    kind VARCHAR(32) NOT NULL
);

CREATE TABLE company_name (
    id INTEGER NOT NULL PRIMARY KEY,
    name TEXT NOT NULL,
    country_code VARCHAR(255),
    imdb_id INTEGER,
    name_pcode_nf VARCHAR(5),
    name_pcode_sf VARCHAR(5),
    md5sum VARCHAR(32)
);

CREATE TABLE company_type (
    id INTEGER NOT NULL PRIMARY KEY,
    kind VARCHAR(32) NOT NULL
);

CREATE TABLE complete_cast (
    id INTEGER NOT NULL PRIMARY KEY,
    movie_id INTEGER,
    subject_id INTEGER NOT NULL,
    status_id INTEGER NOT NULL
);

CREATE TABLE info_type (
    id INTEGER NOT NULL PRIMARY KEY,
    info VARCHAR(32) NOT NULL
);

CREATE TABLE keyword (
    id INTEGER NOT NULL PRIMARY KEY,
    keyword TEXT NOT NULL,
    phonetic_code VARCHAR(5)
);

CREATE TABLE kind_type (
    id INTEGER NOT NULL PRIMARY KEY,
    kind VARCHAR(15) NOT NULL
);

CREATE TABLE link_type (
    id INTEGER NOT NULL PRIMARY KEY,
    link VARCHAR(32) NOT NULL
);

CREATE TABLE movie_companies (
    id INTEGER NOT NULL PRIMARY KEY,
    movie_id INTEGER NOT NULL,
    company_id INTEGER NOT NULL,
    company_type_id INTEGER NOT NULL,
    note TEXT
);

CREATE TABLE movie_info (
    id INTEGER NOT NULL PRIMARY KEY,
    movie_id INTEGER NOT NULL,
    info_type_id INTEGER NOT NULL,
    info TEXT NOT NULL,
    note TEXT
);

CREATE TABLE movie_info_idx (
    id INTEGER NOT NULL PRIMARY KEY,
    movie_id INTEGER NOT NULL,
    info_type_id INTEGER NOT NULL,
    info TEXT NOT NULL,
    note TEXT
);

CREATE TABLE movie_keyword (
    id INTEGER NOT NULL PRIMARY KEY,
    movie_id INTEGER NOT NULL,
    keyword_id INTEGER NOT NULL
);

CREATE TABLE movie_link (
    id INTEGER NOT NULL PRIMARY KEY,
    movie_id INTEGER NOT NULL,
    linked_movie_id INTEGER NOT NULL,
    link_type_id INTEGER NOT NULL
);

CREATE TABLE name (
    id INTEGER NOT NULL PRIMARY KEY,
    name TEXT NOT NULL,
    imdb_index VARCHAR(12),
    imdb_id INTEGER,
    gender VARCHAR(1),
    name_pcode_cf VARCHAR(5),
    name_pcode_nf VARCHAR(5),
    surname_pcode VARCHAR(5),
    md5sum VARCHAR(32)
);

CREATE TABLE person_info (
    id INTEGER NOT NULL PRIMARY KEY,
    person_id INTEGER NOT NULL,
    info_type_id INTEGER NOT NULL,
    info TEXT NOT NULL,
    note TEXT
);

CREATE TABLE role_type (
    id INTEGER NOT NULL PRIMARY KEY,
    role VARCHAR(32) NOT NULL
);

CREATE TABLE title (
    id INTEGER NOT NULL PRIMARY KEY,
    title TEXT NOT NULL,
    imdb_index VARCHAR(12),
    kind_id INTEGER NOT NULL,
    production_year INTEGER,
    imdb_id INTEGER,
    phonetic_code VARCHAR(5),
    episode_of_id INTEGER,
    season_nr INTEGER,
    episode_nr INTEGER,
    series_years VARCHAR(49),
    md5sum VARCHAR(32)
);

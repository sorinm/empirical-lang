This is an extension of the regression tests from `doc/tutorial_running.md`.

### Preparation

Most of this comes from `doc/tutorial_running.md`.

```
>>> let prices = load$("sample_csv/small_prices.csv")

>>> data Reporting: date: Date, quarter: String end

>>> let reports = !Reporting([Date("2017-01-01"), Date("2017-01-09")], ["Q1", "Q2"])

>>> let p = sort prices by date

>>> let near_reports = !Reporting([Date("2017-01-01"), Date("2017-01-05"), Date("2017-01-09")], ["Q1", "Q2", "Q3"])

>>> data FullReporting: symbol: String, date: Date, quarter: String end

>>> let full_reports = !FullReporting(["AAPL", "BRK.B", "EBAY", "AAPL", "BRK.B"], [Date("2017-01-01"), Date("2017-01-01"), Date("2017-01-04"), Date("2017-01-06"), Date("2017-01-11")], ["Q1", "Q1", "Q2", "Q2", "Q2"])

```

### Asof Join

We can combine `strict` and `within`.

```
>>> join p, reports asof date within 3d strict
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q1
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q1
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q1
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q1
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q1
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q1
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587        
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464        
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195        
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900        
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027        
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423        
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948        
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674        
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655        
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051      Q2
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259      Q2
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143      Q2
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593      Q2
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859      Q2
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999      Q2

```

The `within` parameter must be the same type as a subtraction between the `asof` columns.

```
>>> join p, reports asof date within 3
Error: join 'asof' types not compatible with 'within': expected Timedelta, got Int64

```

An `asof` join looks `backward` by default, but we can change the direction to `forward`, meaning we'll match against the first row in the right table whose value is *greater than or equal to* the value in each row of the left table.

```
>>> let next_reports = !Reporting([Date("2017-01-05"), Date("2017-01-10")], ["Q1", "Q2"])

>>> join p, next_reports asof date forward
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q1
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q1
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q1
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q1
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q1
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q1
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587      Q1
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464      Q1
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195      Q1
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900      Q2
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027      Q2
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423      Q2
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948      Q2
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q2
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655      Q2
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051      Q2
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259      Q2
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143      Q2
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859        
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```

Of course we can match `strict` and `forward` for a value that is *strictly greater than*.

```
>>> join p, next_reports asof date forward strict
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q1
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q1
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q1
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q1
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q1
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q1
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587      Q2
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464      Q2
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195      Q2
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900      Q2
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027      Q2
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423      Q2
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948      Q2
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q2
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655      Q2
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051        
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259        
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143        
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859        
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```

We can mix `forward` and `within`.

```
>>> join p, next_reports asof date forward within 3d
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q1
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q1
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q1
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q1
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q1
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q1
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587      Q1
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464      Q1
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195      Q1
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900        
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027        
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423        
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948      Q2
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q2
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655      Q2
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051      Q2
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259      Q2
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143      Q2
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859        
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```

And we can have the trifecta of `strict`, `forward`, and `within`.

```
>>> join p, next_reports asof date forward within 3d strict
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q1
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q1
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q1
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q1
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q1
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q1
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587        
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464        
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195        
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900        
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027        
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423        
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948      Q2
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q2
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655      Q2
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051        
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259        
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143        
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859        
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```

We can combine `nearest` and `within` for a kind of "fuzzy" match.

```
>>> join p, near_reports asof date nearest within 1d
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865        
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967        
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031        
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q2
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q2
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q2
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587      Q2
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464      Q2
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195      Q2
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900      Q2
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027      Q2
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423      Q2
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948      Q3
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q3
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655      Q3
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051      Q3
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259      Q3
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143      Q3
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859        
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```

A `nearest` join cannot be `strict` since matches could be out of order.

```
>>> join p, near_reports asof date nearest strict
Error: join 'asof' cannot be both 'nearest' and 'strict'

```

### Asof/On Join

As expected, the `forward` direction is possible.

```
>>> join p, full_reports on symbol asof date forward
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q2
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q2
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q2
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q2
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q2
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q2
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587      Q2
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464      Q2
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195        
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900      Q2
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027      Q2
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423        
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948        
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q2
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655        
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051        
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259      Q2
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143        
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859      Q2
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```

And the `nearest` is available.

```
>>> join p, full_reports on symbol asof date nearest
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q1
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q1
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q2
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q2
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q1
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q2
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587      Q2
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464      Q1
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195      Q2
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900      Q2
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027      Q1
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423      Q2
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948      Q2
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q2
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655      Q2
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051      Q2
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259      Q2
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143      Q2
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593      Q2
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859      Q2
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999      Q2

```

We can set a `within` tolerance when matching `on` a key.

```
>>> join p, full_reports on symbol asof date within 3d
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q1
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q1
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031        
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q1
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q1
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q2
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587        
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464        
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195      Q2
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900      Q2
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027        
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423      Q2
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948      Q2
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674        
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655        
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051        
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259        
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143        
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859      Q2
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```

The `forward` direction can, of course, have a `within` tolerance while matching `on` a key.

```
>>> join p, full_reports on symbol asof date forward within 3d
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q2
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967        
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q2
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q2
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919        
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q2
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587      Q2
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464        
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195        
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900      Q2
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027        
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423        
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948        
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q2
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655        
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051        
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259      Q2
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143        
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859      Q2
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```

And the `nearest` also permits a `within` when matching `on` a key. Again, this is useful for a "fuzzy" match.

```
>>> join p, full_reports on symbol asof date nearest within 3d
 symbol       date   open   high    low  close   volume quarter
   AAPL 2017-01-03 115.80 116.33 114.76 116.15 28781865      Q1
  BRK.B 2017-01-03 164.34 164.71 162.44 163.83  4090967      Q1
   EBAY 2017-01-03  29.83  30.19  29.64  29.84  7665031      Q2
   AAPL 2017-01-04 115.85 116.51 115.75 116.02 21118116      Q2
  BRK.B 2017-01-04 164.45 164.57 163.00 164.08  3568919      Q1
   EBAY 2017-01-04  29.91  30.01  29.51  29.76  9538779      Q2
   AAPL 2017-01-05 115.92 116.86 115.81 116.61 22193587      Q2
  BRK.B 2017-01-05 164.06 164.14 162.18 163.30  2982464        
   EBAY 2017-01-05  29.73  30.08  29.61  30.01  9062195      Q2
   AAPL 2017-01-06 116.78 118.16 116.47 117.91 31751900      Q2
  BRK.B 2017-01-06 163.44 163.80 162.64 163.41  2697027        
   EBAY 2017-01-06  29.97  31.16  29.78  31.05 13351423      Q2
   AAPL 2017-01-09 117.95 119.43 117.94 118.99 33561948      Q2
  BRK.B 2017-01-09 163.04 163.25 162.02 162.02  3564674      Q2
   EBAY 2017-01-09  31.00  31.03  30.60  30.75 10532655        
   AAPL 2017-01-10 118.77 119.38 118.30 119.11 24462051        
  BRK.B 2017-01-10 162.00 162.74 161.41 161.47  2671259      Q2
   EBAY 2017-01-10  30.67  30.72  29.84  30.25 13833143        
   AAPL 2017-01-11 118.74 119.93 118.60 119.75 27588593        
  BRK.B 2017-01-11 161.54 162.45 161.03 162.23  3305859      Q2
   EBAY 2017-01-11  30.30  30.42  30.01  30.41  8168999        

```
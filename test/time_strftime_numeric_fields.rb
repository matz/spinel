# The numeric fields of strftime written from the time's fields: padding
# (zero, space, none), widths and flags over hours 0, 1, 11, 12, 13 and 23,
# the first and last day of a year and each weekday.
times = [
  Time.utc(2024, 1, 1, 0, 0, 0), Time.utc(2024, 1, 7, 1, 2, 3), Time.utc(2024, 2, 29, 11, 59, 59),
  Time.utc(2024, 12, 31, 12, 30, 5), Time.utc(2023, 12, 31, 13, 0, 9), Time.utc(2025, 6, 15, 23, 45, 6),
  Time.utc(1999, 7, 4, 9, 8, 7), Time.utc(2000, 3, 1, 10, 10, 10)
]
dirs = %w[d e H k I l M S m j u w]
times.each do |t|
  dirs.each do |d|
    p [d, t.strftime("%#{d}"), t.strftime("%-#{d}"), t.strftime("%_#{d}"), t.strftime("%0#{d}"), t.strftime("%5#{d}"), t.strftime("%-5#{d}")]
  end
  p t.strftime("%Y-%m-%d %H:%M:%S.%L %z"), t.strftime("%e %k %l %I%p %j %u %w")
end

# Concurrent UTC, fixed-offset and local conversions must not borrow libc's
# shared struct tm. Each worker's minute differs, so a borrowed result cannot
# accidentally match. Set the zone before starting any threads.
ENV["TZ"] = "EST5EDT,M3.2.0,M11.1.0"
ENV["SPINEL_WORKERS"] = "8"

threads = 8.times.map do |i|
  Thread.new(i) do |n|
    utc = Time.utc(2026, 7, 1, 12, n * 7, n)
    fixed = utc.getlocal("+05:30")
    local = utc.getlocal
    wrong = 0
    20000.times do
      wrong += 1 unless utc.min == n * 7 && utc.hour == 12 && utc.sec == n
      wrong += 1 unless fixed.min == (n * 7 + 30) % 60 && fixed.hour == 17 + (n * 7 + 30) / 60
      wrong += 1 unless local.min == n * 7 && local.hour == 8 && local.utc_offset == -14400
    end
    wrong
  end
end
puts threads.map { |t| t.value }.sum

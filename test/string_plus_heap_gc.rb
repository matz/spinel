# spinel: not-cruby -- GC.stat["cycle"] is Spinel's count of collections; CRuby has no "cycle" key
# Regression for #1450. Appending to a String with + repeatedly allocates
# new Strings. This workload keeps little live data and must collect.
before = GC.stat["cycle"]
s = ""
i = 0
while i < 50000
  s = s + "x"          # fresh result string each step; old s becomes garbage
  s = "" if s.length > 1000   # keep the live set tiny -- all the churn is collectable
  i += 1
end
after = GC.stat["cycle"]
puts(after > before ? "collected" : "NO COLLECTION")

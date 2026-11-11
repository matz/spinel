# An optional parameter defaulting to nil that another call passes a Time: the
# slot is an sp_Time pointer, NULL for nil, and if / unless / nil? / || / &&
# / ?: read its nil (#11343)
def stamp(t = nil)
  if t
    t.getutc.year
  else
    0
  end
end
p stamp(Time.at(0))
p stamp
def n?(t = nil) = t.nil?
p n?(Time.at(0))
p n?
def dflt(t = nil) = (t || Time.at(86400)).utc.day
p dflt(Time.at(0))
p dflt
def un(t = nil)
  return :none unless t
  t.utc.year
end
p un(Time.at(0))
p un
def tern(t = nil) = t ? :time : :nil
p tern(Time.now)
p tern
def both(t = nil) = t && t.utc.year
p both(Time.at(0))
p both
h = { t: nil }
h[:t] = Time.at(0) if ARGV.size > 5
p [h[:t]].first.nil?

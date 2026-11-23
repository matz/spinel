require_relative "range_local_other_file_lib"
def pick(t, flag)
  flag ? t : 3
end
month = pick(Time.at(0).utc, true)..pick(Time.at(10).utc, true)
p month
p offset_of(1)

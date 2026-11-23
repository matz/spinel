# a regex named capture writes a block parameter that an outer local of the same name shadows: the group is named by what the program wrote
m = :outer
["s"].each { |m| if /(?<m>\d+)/ =~ "n42"; p m; end }
p m

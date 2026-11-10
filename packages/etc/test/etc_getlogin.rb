# Etc.getlogin answers the login name of the process, a String, or nil
# where the system knows none (a container with no utmp and no USER).
require "etc"

l = Etc.getlogin
p l.nil? || (l.is_a?(String) && !l.empty?)

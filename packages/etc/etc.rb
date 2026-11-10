# Spinel bundled `etc` -- a carried-C spin package (Path B).
#
# The part of CRuby's Etc that a shell needs: the login name, a user or a
# group by name or by id, and a walk over the two databases. The C
# (sp_etc.c, linked only when `require "etc"` appears) reads an entry with
# getpwnam(3) and its kin and hands it over as one String, the fields
# separated by a unit separator and, for a walk, the entries by a record
# separator; the Struct is built here. A group's members cross as one
# field, joined by commas, as a name holds none.
#
# Not here: uname, nprocessors, sysconf, confstr, sysconfdir and systmpdir,
# and the forms of passwd and group without a block, which in CRuby step
# through the database one call at a time.
module Etc
  module Native
    native_lib "etc"
    native_obj "packages/etc/sp_etc.o"
    native_func :getlogin,   [],        :nstring, "sp_etc_getlogin"
    native_func :getpwnam,   [:string], :nstring, "sp_etc_getpwnam"
    native_func :getpwuid,   [:int],    :nstring, "sp_etc_getpwuid"
    native_func :getgrnam,   [:string], :nstring, "sp_etc_getgrnam"
    native_func :getgrgid,   [:int],    :nstring, "sp_etc_getgrgid"
    native_func :passwd_all, [],        :string,  "sp_etc_passwd_all"
    native_func :group_all,  [],        :string,  "sp_etc_group_all"
  end

  # The portable members of CRuby's structs. CRuby adds change, uclass
  # and expire on a BSD; a program that reads those is not portable in
  # CRuby either.
  Passwd = Struct.new(:name, :passwd, :uid, :gid, :gecos, :dir, :shell)
  Group = Struct.new(:name, :passwd, :gid, :mem)

  FIELD_SEPARATOR = "\x1f"
  RECORD_SEPARATOR = "\x1e"

  def self.__passwd_of(rec)
    f = rec.split(FIELD_SEPARATOR, -1)
    Passwd.new(f[0], f[1], f[2].to_i, f[3].to_i, f[4], f[5], f[6])
  end

  def self.__group_of(rec)
    f = rec.split(FIELD_SEPARATOR, -1)
    Group.new(f[0], f[1], f[2].to_i, f[3].empty? ? [] : f[3].split(","))
  end

  # The login name: getlogin(3), else $USER, else nil, as CRuby answers.
  def self.getlogin
    Native.getlogin
  end

  def self.getpwnam(name)
    rec = Native.getpwnam(name)
    raise ArgumentError, "can't find user for #{name}" unless rec
    __passwd_of(rec)
  end

  def self.getpwuid(uid = Process.uid)
    rec = Native.getpwuid(uid)
    raise ArgumentError, "can't find user for #{uid}" unless rec
    __passwd_of(rec)
  end

  def self.getgrnam(name)
    rec = Native.getgrnam(name)
    raise ArgumentError, "can't find group for #{name}" unless rec
    __group_of(rec)
  end

  def self.getgrgid(gid = Process.gid)
    rec = Native.getgrgid(gid)
    raise ArgumentError, "can't find group for #{gid}" unless rec
    __group_of(rec)
  end

  # Each entry of the user database, from the first; nil, as CRuby's.
  def self.passwd
    raise ArgumentError, "Etc.passwd needs a block in spinel" unless block_given?
    Native.passwd_all.split(RECORD_SEPARATOR).each { |rec| yield __passwd_of(rec) }
    nil
  end

  # Each entry of the group database, from the first; nil, as CRuby's.
  def self.group
    raise ArgumentError, "Etc.group needs a block in spinel" unless block_given?
    Native.group_all.split(RECORD_SEPARATOR).each { |rec| yield __group_of(rec) }
    nil
  end
end

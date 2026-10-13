# Spinel bundled `tempfile` -- a file with a unique name, opened for
# reading and writing, and removed when the program is done with it.
#
# `create` WITH A BLOCK IS THE FORM TO REACH FOR, as CRuby's own
# documentation says: the block is the file's lifetime written down, and
# the file is gone when it ends.
#
# `new` AND `open` HAND BACK A TEMPFILE, which keeps its file until
# `close!` or `unlink`, or until the program exits. CRuby removes a
# dropped Tempfile's file from a finalizer when the object is collected,
# and again at exit for any still alive; spinel has no
# `ObjectSpace.define_finalizer`, so the exit sweep is the whole of it
# here. The files removed are the same ones by the time the program
# ends -- only a Tempfile the program drops without `close!` keeps its
# file longer than it would under CRuby. A forked child removes nothing
# its parent made, as CRuby's finalizer checks the pid.
#
# A Tempfile is not a File, as CRuby's is not (it delegates to one): the
# IO methods below are forwarded to the File it holds, and `to_io`
# answers that File, which is how `$stdout.reopen(tempfile)` finds it.
#
# THE EXCLUSIVE CREATE IS THE POINT, not the unique name. The name is
# guessable -- it carries the date, the pid and 32 bits of `rand`, the
# same shape CRuby's `Dir::Tmpname` builds -- so what keeps a caller
# from being handed a file someone else prepared is O_EXCL, which the
# "x" mode character asks for: a name that already exists is an
# Errno::EEXIST and another attempt, never a handle onto their file.
#
# NOT ACCEPTED, and each for a reason in the runtime rather than in the
# corpus:
#
# * `mode:`, CRuby's extra integer open flags. `File::RDWR` and its
#   siblings are not defined in spinel, so a caller has nothing to pass
#   and nothing to combine. `binmode:` is accepted instead, which is
#   the one option of the set that means anything on a POSIX open.
# * An `encoding:`/`textmode:` pair, for the same reason.
#
# THE ONE DIVERGENCE WORTH NAMING. CRuby's block form asks
# `File.identical?(file, file.path)` before it unlinks, so a block that
# renamed something else onto its own temp path does not have that
# other file removed underneath it. `File.identical?` here takes two
# paths and not an open file, so the question cannot be put; this
# closes and then unlinks by name. Reaching the difference takes a
# block that replaced its own temp file, which is not an accident a
# caller has.
require "tmpdir"

class Tempfile
  # CRuby's Dir::Tmpname retry bound, and the same one packages/tmpdir
  # uses for directories.
  MAX_TRY = 10000

  # The paths of the files `new` made and nothing has unlinked yet, each
  # with the pid that made it. Paths and not the Tempfiles, so a dropped
  # Tempfile is still collected and its descriptor closed.
  @@pending = {}

  # Registered when the package loads, so it runs after every at_exit the
  # program sets up from then on (handlers run newest first), as CRuby's
  # finalizers run after them all. One set up before `require "tempfile"`
  # runs after the sweep.
  at_exit do
    @@pending.each do |path, pid|
      next unless pid == Process.pid
      begin
        File.unlink(path)
      rescue Errno::ENOENT
        # Removed or renamed by the program. Nothing left to do.
      end
    end
  end

  # Tempfile.open("x") { |t| ... } is Tempfile.new with a block that the
  # file is closed after, but NOT removed: CRuby leaves that to the
  # finalizer, and so to the exit sweep here. It answers the block's value;
  # without a block, the Tempfile.
  def self.open(basename = "", tmpdir = nil, max_try: nil, binmode: false, perm: 0600)
    tempfile = new(basename, tmpdir, max_try: max_try, binmode: binmode, perm: perm)
    return tempfile unless block_given?
    begin
      yield tempfile
    ensure
      tempfile.close
    end
  end

  # `perm:` is accepted and IGNORED, which is what CRuby does with it: its
  # own create sets `opts[:perm] = 0600` after merging the caller's options,
  # so the file is owner-only whatever the caller asked for. Honouring it
  # here would hand a caller a group- or world-readable temp file out of a
  # method whose whole contract is that nobody else can open it, and would
  # answer differently from CRuby for the same program.
  def self.create(basename = "", tmpdir = nil, max_try: nil, binmode: false, perm: 0600, &block)
    file = create_file(basename, tmpdir, max_try, binmode)
    return file unless block

    path = file.path
    begin
      yield file
    ensure
      # CLOSE AND UNLINK, BOTH IN THE ENSURE. A block that raised still
      # holds an open descriptor and still has a file on disk, and a
      # test suite that leaks one per assertion runs a machine out of
      # both. A caller that closed the file itself is the ordinary case
      # and not an error.
      file.close unless file.closed?
      begin
        File.unlink(path)
      rescue Errno::ENOENT
        # The block removed or renamed it. Nothing left to do.
      end
    end
  end

  # The open File for a new, unique, owner-only file: what create and new
  # both start from.
  def self.create_file(basename, tmpdir, max_try, binmode)
    prefix, suffix = split_basename(basename)
    parent = tmpdir ? File.path(tmpdir) : Dir.tmpdir
    raise ArgumentError, "empty parent path" if parent.empty?

    tries = max_try || MAX_TRY
    raise ArgumentError, "max_try must be positive" if tries < 1

    # "w" so the open creates, "+" so the caller can read back what it
    # wrote -- a temp file written and then handed to something that
    # reads it is the ordinary use -- and "x" for O_EXCL. The mode
    # truncates, which on a file that cannot already exist is nothing.
    mode = binmode ? "wbx+" : "wx+"
    date = Time.now.strftime("%Y%m%d")

    file = nil
    i = 0
    while i < tries
      # CRuby appends the counter only from the second attempt, so the
      # common case is the name without one.
      counter = i == 0 ? "" : "-#{i}"
      path = "#{parent}/#{prefix}#{date}-#{Process.pid}-" \
             "#{rand(0x100000000).to_s(36)}#{counter}#{suffix}"
      begin
        file = File.open(path, mode, 0600)
        break
      rescue Errno::EEXIST
        i += 1
      end
    end
    raise Errno::EEXIST, "cannot generate temporary file name" if file.nil?
    file
  end

  # `"probe"` or `%w[probe .img]` -- CRuby accepts either, and the array
  # form is how a caller asks for a SUFFIX, which matters when whatever
  # reads the file back looks at its extension.
  def self.split_basename(basename)
    if basename.is_a?(Array)
      prefix = basename.length > 0 ? basename[0].to_s : ""
      suffix = basename.length > 1 ? basename[1].to_s : ""
      return [prefix, suffix]
    end
    [basename.to_s, ""]
  end

  # `perm:` is accepted and ignored, as create's is.
  def initialize(basename = "", tmpdir = nil, max_try: nil, binmode: false, perm: 0600)
    @tmpfile = Tempfile.create_file(basename, tmpdir, max_try, binmode)
    @binmode = binmode
    @unlinked = false
    @@pending[@tmpfile.path] = Process.pid
  end

  # The path, or nil once the file is unlinked.
  def path
    @unlinked ? nil : @tmpfile.path
  end

  # Remove the file now, answering true. The Tempfile stays open and
  # usable; only its name is gone, as on any POSIX file.
  def unlink
    return if @unlinked
    path = @tmpfile.path
    begin
      File.unlink(path)
    rescue Errno::ENOENT
      # Removed or renamed by the program already.
    end
    @@pending.delete(path)
    @unlinked = true
  end
  alias delete unlink

  def close(unlink_now = false)
    @tmpfile.close
    unlink_now ? unlink : nil
  end

  def close!
    close(true)
  end

  # Reopen the closed file for reading and writing, from the start, and
  # answer self. Its content is kept: CRuby drops the create and exclusive
  # flags here, and keeps binmode.
  def open
    @tmpfile.close unless @tmpfile.closed?
    @tmpfile = File.open(@tmpfile.path, "r+")
    @tmpfile.binmode if @binmode
    self
  end

  # The size, flushed first; a closed Tempfile asks the file on disk.
  def size
    return File.size(@tmpfile.path) if @tmpfile.closed?
    @tmpfile.flush
    @tmpfile.size
  end
  alias length size

  def inspect
    @tmpfile.closed? ? "#<#{self.class}:#{path} (closed)>" : "#<#{self.class}:#{path}>"
  end
  alias to_s inspect

  # The File the Tempfile holds: what IO#reopen and IO.select take from it.
  def to_io = @tmpfile
  def to_path = @tmpfile.to_path
  def closed? = @tmpfile.closed?

  # The IO surface, each method forwarded to the File with the arguments
  # it was given, answering what the File answers.
  def read(*args) = @tmpfile.read(*args)
  def readpartial(maxlen) = @tmpfile.readpartial(maxlen)
  def gets(*args, **opts) = @tmpfile.gets(*args, **opts)
  def readline(*args, **opts) = @tmpfile.readline(*args, **opts)
  def readlines(*args, **opts) = @tmpfile.readlines(*args, **opts)
  # A literal block, not &block, and readlines, not a blockless each_line:
  # File#each_line is lowered only with a literal block.
  def each_line(*args, **opts)
    return @tmpfile.readlines(*args, **opts).each unless block_given?

    @tmpfile.each_line(*args, **opts) { |line| yield line }
  end
  def each(*args, **opts, &block) = each_line(*args, **opts, &block)
  def each_char(&block) = @tmpfile.each_char(&block)
  def each_byte(&block) = @tmpfile.each_byte(&block)
  def getc = @tmpfile.getc
  def getbyte = @tmpfile.getbyte
  def readchar = @tmpfile.readchar
  def readbyte = @tmpfile.readbyte
  def ungetc(char) = @tmpfile.ungetc(char)
  def write(*objects) = @tmpfile.write(*objects)
  def print(*objects) = @tmpfile.print(*objects)
  def puts(*objects) = @tmpfile.puts(*objects)
  def printf(format, *args) = @tmpfile.printf(format, *args)
  def putc(object) = @tmpfile.putc(object)
  def <<(object) = @tmpfile << object
  def rewind = @tmpfile.rewind
  def seek(offset, whence = IO::SEEK_SET) = @tmpfile.seek(offset, whence)
  def pos = @tmpfile.pos
  def tell = @tmpfile.tell
  def pos=(offset)
    @tmpfile.pos = offset
  end
  def eof? = @tmpfile.eof?
  def eof = @tmpfile.eof
  def lineno = @tmpfile.lineno
  def lineno=(number)
    @tmpfile.lineno = number
  end
  def flush = @tmpfile.flush
  def fsync = @tmpfile.fsync
  def sync = @tmpfile.sync
  def sync=(mode)
    @tmpfile.sync = mode
  end
  def fileno = @tmpfile.fileno
  def binmode = @tmpfile.binmode
  def binmode? = @tmpfile.binmode?
  def truncate(length) = @tmpfile.truncate(length)
  def stat = @tmpfile.stat
  def chmod(mode) = @tmpfile.chmod(mode)
  def mtime = @tmpfile.mtime
end

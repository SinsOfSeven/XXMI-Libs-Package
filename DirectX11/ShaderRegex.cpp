#include "ShaderRegex.h"
#include "ShaderStage.h"
#include "ShaderStore.h"
#include "CommandList.h"
#include "globals.h" // For ShaderOverride FIXME: This should be in a separate header
#include "log.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>

ShaderRegexGroups shader_regex_groups;
std::vector<ShaderRegexGroup*> shader_regex_group_index;
uint32_t shader_regex_hash;

static void log_pcre2_error_nonl(int err, char *fmt, ...)
{
	PCRE2_UCHAR buf[120]; // doco says "120 code units is ample"
	va_list ap;

	pcre2_get_error_message(err, buf, sizeof(buf));

	va_start(ap, fmt);
	vLogInfo(fmt, ap);
	va_end(ap);

	LogInfo(": %s\n", buf);
}

static bool get_shader_model(std::string *asm_text, std::string *shader_model)
{
	size_t shader_model_pos;

	for (
		shader_model_pos = asm_text->find("\n");
		shader_model_pos != std::string::npos && (*asm_text)[shader_model_pos + 1] == '/';
		shader_model_pos = asm_text->find("\n", shader_model_pos + 1)
	) {}

	if (shader_model_pos == std::string::npos)
		return false;

	*shader_model = asm_text->substr(shader_model_pos + 1, asm_text->find("\n", shader_model_pos + 1) - shader_model_pos - 1);
	return true;
}

static bool find_dcl_end(std::string *asm_text, size_t *dcl_end_pos)
{
	// FIXME: Might be better to scan forwards

	*dcl_end_pos = asm_text->rfind("\ndcl_");
	*dcl_end_pos = asm_text->find("\n", *dcl_end_pos + 1);

	if (*dcl_end_pos == std::string::npos) {
		LogInfo("WARNING: Unable to locate end of shader declarations!\n");
		return false;
	}

	return true;
}

static bool insert_declarations(std::string *asm_text, ShaderRegexDeclarations *declarations)
{
	ShaderRegexDeclarations::iterator i;
	std::string insert_str;
	size_t dcl_end;
	bool patch = false;

	if (!find_dcl_end(asm_text, &dcl_end))
		return false;

	for (i = declarations->begin(); i != declarations->end(); i++) {
		insert_str = std::string("\n") + *i;

		if (asm_text->find(insert_str + std::string("\n")) != std::string::npos)
			continue;

		asm_text->insert(dcl_end, insert_str);
		dcl_end += insert_str.size();

		patch = true;
	}

	return patch;
}

static bool find_dcl_temps(std::string *asm_text, size_t *dcl_temps_pos)
{
	// Could use regex for this as well, but given we only need to find a
	// constant string it will be more efficient to just do this:
	*dcl_temps_pos = asm_text->find("\ndcl_temps ", 0);

	if (*dcl_temps_pos == std::string::npos)
		return false;

	return true;
}

static unsigned get_dcl_temps(std::string *asm_text)
{
	size_t dcl_temps;
	unsigned tmp_regs = 0;

	if (!find_dcl_temps(asm_text, &dcl_temps))
		return 0;

	tmp_regs = stoul(asm_text->substr(dcl_temps + 10, 4));
	LogInfo("Found dcl_temps %d\n", tmp_regs);

	return tmp_regs;
}

static bool update_dcl_temps(std::string *asm_text, size_t new_val)
{
	size_t dcl_temps, dcl_temps_end, dcl_end;
	std::string insert_str;

	if (find_dcl_temps(asm_text, &dcl_temps)) {
		dcl_temps += 11;
		dcl_temps_end = asm_text->find("\n", dcl_temps);
		LogInfo("Updating dcl_temps %Iu\n", new_val);
		asm_text->replace(dcl_temps, dcl_temps_end - dcl_temps, std::to_string(new_val));
		return true;
	}

	if (!find_dcl_end(asm_text, &dcl_end))
		return false;

	insert_str = std::string("\ndcl_temps ") + std::to_string(new_val);
	LogInfo("Inserting dcl_temps %Iu\n", new_val);
	asm_text->insert(dcl_end, insert_str);
	dcl_end += insert_str.size();

	return true;
}

ShaderRegexPattern::ShaderRegexPattern() :
	regex(NULL),
	do_replace(false)
{
}

ShaderRegexPattern::~ShaderRegexPattern()
{
	pcre2_code_free(regex);
}

bool ShaderRegexPattern::compile(std::string *pattern)
{
	uint32_t name_table_entry_size;
	uint32_t name_table_count;
	uint32_t i;
	PCRE2_SPTR name_table;
	PCRE2_SIZE err_off;
	int err;

	// CASELESS is for compatibility with d3dcompiler_46 & 47 without
	// having to always remember to account for the dcl_constantbuffer
	// differences:
	regex = pcre2_compile((PCRE2_SPTR)pattern->c_str(),
			pattern->length(), // or PCRE2_ZERO_TERMINATED
			PCRE2_CASELESS | PCRE2_MULTILINE,
			&err, &err_off, NULL);
	if (!regex) {
		log_pcre2_error_nonl(err, "  WARNING: PCRE2 regex compilation failed at offset %u", (unsigned)err_off);
		return false;
	}

	// TODO: Use callback to confirm that JIT does actually get used, as in
	// some cases pcre2 can fall back to using the slower interpreter
	pcre2_jit_compile(regex, 0);

	pcre2_pattern_info(regex, PCRE2_INFO_NAMECOUNT, &name_table_count);
	pcre2_pattern_info(regex, PCRE2_INFO_NAMEENTRYSIZE, &name_table_entry_size);
	pcre2_pattern_info(regex, PCRE2_INFO_NAMETABLE, &name_table);

	static_assert(PCRE2_CODE_UNIT_WIDTH == 8, "Need to fix name table parsing for non-8bit pcre2");
	for (i = 0; i < name_table_count; i++)
		named_capture_groups.insert(std::string((char*)(name_table + name_table_entry_size*i + 2)));

	return true;
}

bool ShaderRegexPattern::named_group_overlaps(ShaderRegexTemps &other_set)
{
	ShaderRegexTemps intersection;

	// C++ why you be so verbose?
	std::set_intersection(
				named_capture_groups.begin(),
				named_capture_groups.end(),
				other_set.begin(),
				other_set.end(),
				std::inserter(intersection, intersection.begin()));

	return intersection.size() != 0;
}

bool ShaderRegexPattern::matches(std::string *asm_text)
{
	pcre2_match_data *match_data = NULL;
	bool match = false;
	int rc;

	// TODO: Assign per-thread JIT stack if the default 32K turns out to be
	// insufficient. Can probably store this in the context, as that is
	// supposed to be per-thread, or use thread local storage.

	match_data = pcre2_match_data_create_from_pattern(regex, NULL);

	// TODO: Consider using pcre2_jit_match - doco claims 10% faster, but
	// has less sanity checks. TODO: Use callback to confirm JIT was used
	rc = pcre2_match(regex, (PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0, 0, match_data, NULL);
	if (rc == PCRE2_ERROR_NOMATCH)
		goto out_free;
	if (rc < 0) {
		log_pcre2_error_nonl(rc, "  WARNING: regex match error");
		goto out_free;
	}

	match = true;

out_free:
	pcre2_match_data_free(match_data);
	return match;
}

static void replacement_search_and_replace(std::string &str, std::string *search, std::string *replace)
{
	size_t pos;

	for (pos = str.find(*search); pos != std::string::npos; pos = str.find(*search, pos + 1)) {
		if (pos > 0 && (str[pos-1] == '$' || str[pos-1] == '\\'))
			continue;

		str.replace(pos, search->length(), *replace);
	}
}

static void substitute_temp_regs(std::string &replacement, ShaderRegexTemps *temp_regs, unsigned dcl_temps)
{
	ShaderRegexTemps::iterator i;
	unsigned tmp_reg = dcl_temps;
	std::string search_str, repl_str;

	for (i = temp_regs->begin(); i != temp_regs->end(); i++, tmp_reg++) {
		repl_str = std::string("r") + std::to_string(tmp_reg);

		search_str = std::string("$") + *i;
		replacement_search_and_replace(replacement, &search_str, &repl_str);

		search_str = std::string("${") + *i + std::string("}");
		replacement_search_and_replace(replacement, &search_str, &repl_str);
	}
}

bool ShaderRegexPattern::patch(std::string *asm_text, ShaderRegexTemps *temp_regs, unsigned dcl_temps)
{
	pcre2_match_data *match_data = NULL;
	PCRE2_SIZE est_size, output_size;
	std::string replace_copy;
	PCRE2_UCHAR *buf = NULL;
	bool patch = false;
	uint32_t options;
	int rc;

	static_assert(PCRE2_CODE_UNIT_WIDTH == 8, "Need to fix output buffer allocation for non-8bit pcre2");

	// We operate on a copy of the replace string so that future shaders
	// don't get our temporary register numbers:
	replace_copy = replace;
	substitute_temp_regs(replace_copy, temp_regs, dcl_temps);

	// TODO: Allow named capture groups from other patterns in the same
	// regex group to be substituted in, and provide some simple arithmetic
	// operators to e.g. allow a constant buffer byte offset to be divided
	// by 16 to get the constant buffer index and vice versa

	// At a minimum we want \n to be translated in the replace string,
	// which needs extended substitution processing to be enabled:
	options = PCRE2_SUBSTITUTE_EXTENDED;

	match_data = pcre2_match_data_create_from_pattern(regex, NULL);

	output_size = est_size = asm_text->length() + replace_copy.length() + 1024;
	buf = new PCRE2_UCHAR[output_size];
	rc = pcre2_substitute(regex,
			(PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0,
			options | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
			match_data, NULL,
			(PCRE2_SPTR)replace_copy.c_str(), replace_copy.length(),
			buf, &output_size);

	if (rc == PCRE2_ERROR_NOMEMORY) {
		LogInfo("  NOTICE: regex replace requires a %u byte buffer\n", (unsigned)output_size);
		LogInfo("  NOTICE: We underestimated by %u bytes and have to start over\n", (unsigned)(output_size - est_size));
		LogInfo("  NOTICE: What kind of crazy are you doing to get down this code path?\n");
		LogInfo("  NOTICE: You didn't inject a matrix inverse or two in assembly did you?\n");
		LogInfo("  NOTICE: Once more, with passion!\n");

		delete [] buf;
		buf = new PCRE2_UCHAR[output_size];

		rc = pcre2_substitute(regex,
				(PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0,
				options, // No PCRE2_SUBSTITUTE_OVERFLOW_LENGTH this time
				match_data, NULL,
				(PCRE2_SPTR)replace_copy.c_str(), replace_copy.length(),
				buf, &output_size);
	}

	if (rc == 0)
		goto out_free;
	if (rc < 0) {
		log_pcre2_error_nonl(rc, "  WARNING: regex replace error");
		goto out_free;
	}

	*asm_text = (char*)buf;
	patch = true;

out_free:
	pcre2_match_data_free(match_data);
	delete [] buf;

	return patch;
}

void ShaderRegexGroup::apply_regex_patterns(std::string *asm_text, bool *match, bool *patch)
{
	ShaderRegexPatterns::iterator i;
	ShaderRegexPattern *pattern;
	unsigned dcl_temps = 0;

	// Match defaults to true so that if there are no patterns we can still
	// apply the command list. Patch defaults to false because we don't
	// want to waste time re-assembling the shader if we didn't change it.
	*match = true;
	*patch = false;

	if (!temp_regs.empty())
		dcl_temps = get_dcl_temps(asm_text);

	for (i = patterns.begin(); i != patterns.end(); i++) {
		pattern = &i->second;

		if (pattern->do_replace)
			*match = *patch = pattern->patch(asm_text, &temp_regs, dcl_temps);
		else
			*match = pattern->matches(asm_text);

		if (!*match) {
			*patch = false;
			return;
		}
	}

	// Only update dcl_temps if we are patching:
	if (*patch && !temp_regs.empty())
		*patch = update_dcl_temps(asm_text, dcl_temps + temp_regs.size());

	// But we can update declarations even if we aren't doing a regex
	// replace in some cases, so long as the patterns all matched (e.g.
	// globally disable the driver stereo cb):
	if (!declarations.empty())
		*patch = insert_declarations(asm_text, &declarations) || *patch;
}

void ShaderRegexGroup::link_command_lists_and_filter_index(UINT64 shader_hash)
{
	ShaderOverride *shader_override = NULL;
	wstring ini_section, ini_line;
	CommandList::Commands::reverse_iterator i;

	// Only link the command lists if we have something in ours to link in,
	// because this will create ShaderOverride sections for shaders that
	// don't already have one, adding more work in the draw calls.

	if (command_list.noop() && post_command_list.noop() && filter_index == FLT_MAX)
		return;

	shader_override = &G->mShaderOverrideMap[shader_hash];

	// Initialise the ShaderOverride's command lists if they aren't already:
	if (shader_override->command_list.ini_section.empty()) {
		ini_section = command_list.ini_section + L".Match";
		shader_override->command_list.ini_section = ini_section;
		shader_override->post_command_list.ini_section = ini_section;
		shader_override->post_command_list.post = true;
	}

	// Set the filter index for partner filtering:
	if (shader_override->filter_index == FLT_MAX)
		shader_override->filter_index = filter_index;

	// If we have previously linked a command list (on any matched shader)
	// we will reuse the link command here, after checking that this
	// matched shader has not already been linked. Avoids the command lists
	// growing endlessly and eventually killing performance.
	if (link) {
		for (i = shader_override->command_list.commands.rbegin();
		         i != shader_override->command_list.commands.rend(); i++) {
			if (*i == link)
				return;
		}
		shader_override->command_list.commands.push_back(link);
		if (post_link)
			shader_override->post_command_list.commands.push_back(post_link);
		return;
	} else if (post_link) {
		for (i = shader_override->post_command_list.commands.rbegin();
		         i != shader_override->post_command_list.commands.rend(); i++) {
			if (*i == post_link)
				return;
		}
		shader_override->post_command_list.commands.push_back(post_link);
		return;
	}

	// This is the first shader this pattern has matched. Create a new
	// RunLinkedCommandList command and link it up:
	ini_line = L"[" + command_list.ini_section + L".Match] run = linked command list";

	if (!command_list.noop())
		link = LinkCommandLists(&shader_override->command_list, &command_list, &ini_line);

	if (!post_command_list.noop())
		post_link = LinkCommandLists(&shader_override->post_command_list, &post_command_list, &ini_line);
}

bool unlink_shader_regex_command_lists_and_filter_index(UINT64 shader_hash)
{
	ShaderOverride *shader_override = NULL;
	CommandList::Commands::iterator i, next;
	RunLinkedCommandList *link;
	bool ret = false;

	auto shader_override_i = G->mShaderOverrideMap.find(shader_hash);
	if (shader_override_i == G->mShaderOverrideMap.end())
		return false;

	shader_override = &shader_override_i->second;

	for (i = shader_override->command_list.commands.begin(), next = i;
	    i != shader_override->command_list.commands.end(); i = next) {
		next++;
		link = dynamic_cast<RunLinkedCommandList*>(i->get());
		if (link) {
			next = shader_override->command_list.commands.erase(i);
			ret = true;
		}
	}

	for (i = shader_override->post_command_list.commands.begin(), next = i;
	    i != shader_override->post_command_list.commands.end(); i = next) {
		next++;
		link = dynamic_cast<RunLinkedCommandList*>(i->get());
		if (link) {
			next = shader_override->post_command_list.commands.erase(i);
			ret = true;
		}
	}

	if (shader_override->filter_index != shader_override->backup_filter_index) {
		shader_override->filter_index = shader_override->backup_filter_index;
		ret = true;
	}

	return ret;
}

#define SHADER_REGEX_CACHE_VERSION 2
#define SHADER_REGEX_CACHE_MAGIC ('S' | ('R' << 8) | ('C' << 16) | ('X' << 24))

// The packed ShaderRegex cache (version 2). Replaces the old per-shader
// "<hash>-<type>_regex.dat/.bin" files. All the metadata for every shader
// lives in a single "ShaderRegexCache.dat" file (a cacheline friendly header,
// a flat array of fixed size records sorted for binary search and a flat pool
// of match ids), which is loaded into memory in one go. The patched bytecode
// lives in a single grow-only "ShaderRegexCache.blob" file made up of 4KiB
// aligned blocks that are streamed through a small sliding window on demand,
// so the whole blob is never loaded into memory and repeated accesses to the
// same block (e.g. after a config reload re-links the command lists) are
// served straight from the window. The blob is grow-only: dead blocks (a
// re-saved record or an unpatched record) are simply skipped over by the
// record offsets, and the whole pair is compacted by wiping whenever the
// ShaderRegex generation changes.
struct ShaderRegexCacheHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t shader_regex_hash;
	uint32_t record_count;
	uint64_t blob_size;
	uint32_t match_pool_count;
	uint32_t flags;
};

struct ShaderRegexCacheRecord {
	uint64_t hash;
	uint32_t match_pool_index;
	uint32_t match_count;
	uint64_t blob_offset;
	uint32_t blob_size;
	ShaderStage shader_type; // single byte, on-disk layout unchanged
	uint8_t patched;
	uint16_t _pad;
};

static bool shader_regex_cache_record_less(const ShaderRegexCacheRecord &a, const ShaderRegexCacheRecord &b)
{
	if (a.shader_type != b.shader_type)
		return a.shader_type < b.shader_type;
	return a.hash < b.hash;
}

static void shader_regex_cache_get_paths(wchar_t *dat_path, wchar_t *blob_path)
{
	// Zero first so a failed swprintf_s can't leave a garbage path behind:
	dat_path[0] = 0;
	if (blob_path)
		blob_path[0] = 0;

	swprintf_s(dat_path, MAX_PATH, L"%ls\\ShaderRegexCache.dat", G->SHADER_CACHE_PATH);
	if (blob_path)
		swprintf_s(blob_path, MAX_PATH, L"%ls\\ShaderRegexCache.blob", G->SHADER_CACHE_PATH);
}

// Holds the packed ShaderRegex cache: the in-memory record index (header +
// sorted fixed-size records + flat match-id pool) and the grow-only bytecode
// blob streamed through a small sliding window. All state here is guarded by
// shader_regex_cache_mutex; the public load/save_meta/save_bin/flush methods
// assume the lock is already held by their free-function wrappers.
class ShaderRegexCacheStore {
public:
	ShaderRegexCache load(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode,
			std::wstring *tagline, ShaderRegexCacheFailureReason *reason);
	void save_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
			bool patched, std::string *asm_text, std::wstring *tagline);
	void save_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode);

	// Writes the metadata out if new records were queued since the last
	// flush. Called from the per-frame seam so the on-disk index stays at
	// most one frame behind the in-memory state (the blob itself is always
	// appended to immediately):
	bool flush();

private:
	bool loaded = false;
	ShaderRegexCacheHeader header;
	std::vector<ShaderRegexCacheRecord> records;
	std::vector<uint32_t> match_pool;
	bool metadata_dirty = false;

	// Grow-only bytecode blob, streamed through a small sliding window (see
	// ShaderStore.h). Shared between the cache stores so the same machinery
	// backs the ShaderBytecodeRecord ledger as well:
	BlobStore blob;

	std::vector<ShaderRegexCacheRecord>::iterator lower_bound(ShaderStage shader_type, uint64_t hash);
	ShaderRegexCacheRecord *find_record(ShaderStage shader_type, uint64_t hash);
	ShaderRegexCacheRecord *find_or_insert_record(ShaderStage shader_type, uint64_t hash);
	bool persist_metadata();
	bool metadata_load(const wchar_t *dat_path);
	bool validate_blob();
	void reset_state();
	void wipe_files();
	bool create_metadata();
	bool ensure_valid(bool create_if_missing);
};

// Serializes access to the cache state. This is required because
// apply_shader_regex_groups() is also called from CopyToFixes() (when a user
// marks a shader) which does not hold the global mCriticalSection, while
// DeferredShaderReplacement() (which does hold it) can be analysing a shader
// on another thread at the same time. Lock order must remain mCriticalSection
// -> shader_regex_cache_mutex; never acquire it in the other direction:
static std::mutex shader_regex_cache_mutex;
static ShaderRegexCacheStore shader_regex_cache_store;

std::vector<ShaderRegexCacheRecord>::iterator ShaderRegexCacheStore::lower_bound(ShaderStage shader_type, uint64_t hash)
{
	ShaderRegexCacheRecord key;
	memset(&key, 0, sizeof(key));
	key.shader_type = shader_type;
	key.hash = hash;

	return std::lower_bound(records.begin(), records.end(),
			key, shader_regex_cache_record_less);
}

ShaderRegexCacheRecord *ShaderRegexCacheStore::find_record(ShaderStage shader_type, uint64_t hash)
{
	std::vector<ShaderRegexCacheRecord>::iterator i = lower_bound(shader_type, hash);

	if (i == records.end()
	 || i->shader_type != shader_type || i->hash != hash)
		return NULL;
	return &*i;
}

ShaderRegexCacheRecord *ShaderRegexCacheStore::find_or_insert_record(ShaderStage shader_type, uint64_t hash)
{
	std::vector<ShaderRegexCacheRecord>::iterator i = lower_bound(shader_type, hash);

	if (i != records.end()
	 && i->shader_type == shader_type && i->hash == hash)
		return &*i;

	ShaderRegexCacheRecord record;
	memset(&record, 0, sizeof(record));
	record.hash = hash;
	record.shader_type = shader_type;

	return &*records.insert(i, record);
}

bool ShaderRegexCacheStore::flush()
{
	if (!metadata_dirty)
		return true;
	return persist_metadata();
}

bool ShaderRegexCacheStore::persist_metadata()
{
	wchar_t dat_path[MAX_PATH];
	size_t size;
	std::vector<byte> out;

	metadata_dirty = false;

	shader_regex_cache_get_paths(dat_path, NULL);

	header.record_count = (uint32_t)records.size();
	header.match_pool_count = (uint32_t)match_pool.size();

	// The metadata is persisted by shader_store_metadata_write() to a temp
	// file that is atomically renamed over the real file so a crash at any
	// point can never leave a half-written cache behind. The blob is always
	// appended to *before* this runs, so the persisted metadata never
	// references bytecode that isn't on disk yet:
	size = sizeof(ShaderRegexCacheHeader)
			+ records.size() * sizeof(ShaderRegexCacheRecord)
			+ match_pool.size() * sizeof(uint32_t);

	out.resize(size);

	memcpy(out.data(), &header, sizeof(ShaderRegexCacheHeader));
	memcpy(out.data() + sizeof(ShaderRegexCacheHeader),
			records.data(),
			records.size() * sizeof(ShaderRegexCacheRecord));
	memcpy(out.data() + sizeof(ShaderRegexCacheHeader)
			+ records.size() * sizeof(ShaderRegexCacheRecord),
			match_pool.data(),
			match_pool.size() * sizeof(uint32_t));

	if (!shader_store_metadata_write(dat_path, out.data(), out.size())) {
		LogWarning("ShaderRegexCache: persist_metadata write to %S FAILED (records=%u match_pool=%u size=%Iu)\n",
				dat_path, header.record_count, header.match_pool_count, size);
		return false;
	}
	return true;
}

bool ShaderRegexCacheStore::metadata_load(const wchar_t *dat_path)
{
	std::vector<byte> buf;

	if (!shader_store_metadata_read(dat_path, &buf))
		return false;

	bool ok = false;

	if (buf.size() >= sizeof(ShaderRegexCacheHeader)) {
		ShaderRegexCacheHeader header;
		memcpy(&header, buf.data(), sizeof(header));

		if (header.magic == SHADER_REGEX_CACHE_MAGIC
		 && header.version == SHADER_REGEX_CACHE_VERSION) {
			// The file must be exactly its expected size (header + fixed
			// size records + flat match id pool) or it's not our file:
			size_t expected = sizeof(ShaderRegexCacheHeader)
					+ (size_t)header.record_count * sizeof(ShaderRegexCacheRecord)
					+ (size_t)header.match_pool_count * sizeof(uint32_t);

			if (buf.size() == expected) {
				this->header = header;
				records.resize(header.record_count);
				match_pool.resize(header.match_pool_count);

				memcpy(records.data(),
						buf.data() + sizeof(ShaderRegexCacheHeader),
						records.size() * sizeof(ShaderRegexCacheRecord));
				memcpy(match_pool.data(),
						buf.data() + sizeof(ShaderRegexCacheHeader)
							+ records.size() * sizeof(ShaderRegexCacheRecord),
						match_pool.size() * sizeof(uint32_t));

				// Validate every record: the (stage, hash) sort invariant
				// the binary search relies on, plus all the pool and blob
				// ranges. Anything off means the file was tampered with or
				// truncated:
				ok = true;
				ShaderRegexCacheRecord prev;
				for (size_t i = 0; i < records.size() && ok; i++) {
					ShaderRegexCacheRecord &r = records[i];

					if (i && !shader_regex_cache_record_less(prev, r))
						ok = false;
					if (r.shader_type >= ShaderStage::COUNT)
						ok = false;
					if (r.match_count
					 && (uint64_t)r.match_pool_index + r.match_count > (uint64_t)header.match_pool_count)
						ok = false;
					if (r.blob_size
					 && r.blob_offset + r.blob_size > (uint64_t)header.blob_size)
						ok = false;

					prev = r;
				}
			}
		}
	}

	return ok;
}

// Make sure the blob is consistent with the metadata before we trust it. The
// blob is grow-only, so it only ever needs to be at least as large as the
// metadata claims:
bool ShaderRegexCacheStore::validate_blob()
{
	return blob.validate(header.blob_size);
}

void ShaderRegexCacheStore::reset_state()
{
	blob.close();

	records.clear();
	match_pool.clear();
	loaded = false;
	metadata_dirty = false;
}

void ShaderRegexCacheStore::wipe_files()
{
	wchar_t dat_path[MAX_PATH], blob_path[MAX_PATH];

	shader_regex_cache_get_paths(dat_path, blob_path);

	reset_state();

	DeleteFile(dat_path);
	DeleteFile(blob_path);
}

bool ShaderRegexCacheStore::create_metadata()
{
	wchar_t dat_path[MAX_PATH];

	shader_regex_cache_get_paths(dat_path, NULL);

	memset(&header, 0, sizeof(ShaderRegexCacheHeader));
	header.magic = SHADER_REGEX_CACHE_MAGIC;
	header.version = SHADER_REGEX_CACHE_VERSION;
	header.shader_regex_hash = shader_regex_hash;
	loaded = true;
	metadata_dirty = true;

	if (!persist_metadata()) {
		LogWarning("ShaderRegexCache: create_metadata failed to persist %S (shader_regex_hash=%08x)\n",
				dat_path, shader_regex_hash);
		reset_state();
		return false;
	}

	LogWarning("ShaderRegexCache: cache created at %S (generation %08x)\n", dat_path, shader_regex_hash);
	return true;
}

// Loads the metadata into memory, or (re)creates it if create_if_missing.
// The cache is tied to the ShaderRegex generation hash: if it doesn't match
// the current shader_regex_hash the whole pair is discarded (and recreated
// from scratch on the next write) so no stale cache is ever used:
bool ShaderRegexCacheStore::ensure_valid(bool create_if_missing)
{
	wchar_t dat_path[MAX_PATH], blob_path[MAX_PATH];

	shader_regex_cache_get_paths(dat_path, blob_path);
	blob.set_path(blob_path);

	// We may already have the cache loaded from a previous config reload. If
	// the ShaderRegex generation changed since then it's all stale:
	if (loaded) {
		if (header.shader_regex_hash == shader_regex_hash) {
			blob.set_logical_end(header.blob_size);
			return true;
		}

		if (!create_if_missing) {
			reset_state();
			return false;
		}

		wipe_files();
	}

	if (metadata_load(dat_path)
	 && header.shader_regex_hash == shader_regex_hash
	 && validate_blob()) {
		blob.set_logical_end(header.blob_size);
		loaded = true;
		LogWarning("ShaderRegexCache: loaded %u records from %S (blob %Iu, generation %08x)\n",
				(unsigned)records.size(), dat_path, (uint64_t)header.blob_size, header.shader_regex_hash);
		return true;
	}

	reset_state();

	if (!create_if_missing)
		return false;

	LogWarning("ShaderRegexCache: %S not present or invalid for generation %08x, (re)creating\n",
			dat_path, shader_regex_hash);
	wipe_files();

	return create_metadata();
}

ShaderRegexCache ShaderRegexCacheStore::load(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode, std::wstring *tagline, ShaderRegexCacheFailureReason *reason)
{
	ShaderRegexCache ret = ShaderRegexCache::NO_CACHE;
	ShaderRegexGroup *group;
	ShaderRegexCacheRecord *record;
	ShaderStage stage;
	uint32_t i;

	if (reason)
		*reason = ShaderRegexCacheFailureReason::NONE;

	// Diagnostic option: never load from the packed cache, forcing the full
	// deferred analysis path (useful for testing without the cache):
	if (G->DISABLE_REGEX_CACHE) {
		if (reason)
			*reason = ShaderRegexCacheFailureReason::DISABLED;
		return ret;
	}

	if (!G->SHADER_CACHE_PATH[0]) {
		if (reason)
			*reason = ShaderRegexCacheFailureReason::NO_CACHE_PATH;
		return ret;
	}

	stage = shader_stage_from_label(shader_type);
	if (stage == ShaderStage::INVALID) {
		if (reason)
			*reason = ShaderRegexCacheFailureReason::INVALID_STAGE;
		return ret;
	}

	if (!ensure_valid(false)) {
		if (reason)
			*reason = ShaderRegexCacheFailureReason::STALE_GENERATION;
		return ret;
	}

	record = find_record(stage, hash);
	if (!record) {
		if (reason)
			*reason = ShaderRegexCacheFailureReason::NO_RECORD;
		return ret;
	}

	// num_matches may be 0, which means the ShaderRegex didn't match the
	// shader, but we cache it anyway to skip processing the shader again.
	// We don't really need any special handling for this case, since
	// returning MATCH will already skip that handling in the caller, but
	// we return a special value so the caller can log it appropriately.
	if (record->match_count == 0)
		return ShaderRegexCache::NO_MATCH;

	for (i = 0; i < record->match_count; i++) {
		// The ShaderRegex groups are sorted and since the cached hash
		// already matched the map should be identical to when the
		// cache was made, so we can use that to find the matching
		// groups without having to do an expensive lookup by name:
		uint32_t match_id = match_pool[record->match_pool_index + i];
		if (match_id >= shader_regex_group_index.size()) {
			if (reason)
				*reason = ShaderRegexCacheFailureReason::MATCH_POOL_OOB;
			return ret;
		}
		group = shader_regex_group_index[match_id];

		LogWarning("ShaderRegexCache: %S %016I64x matches [%S]\n",
				shader_type, hash, group->ini_section.c_str());

		if (record->patched && tagline)
			tagline->append(std::wstring(L"[") + group->ini_section + std::wstring(L"]"));

		group->link_command_lists_and_filter_index(hash);
	}

	if (record->patched) {
		// A blob_size of 0 means the metadata was saved before the bytecode
		// was assembled (e.g. assembly failed last time around) - treat it as
		// a cache miss and re-analyse the shader:
		if (!record->blob_size) {
			if (reason)
				*reason = ShaderRegexCacheFailureReason::RECORD_NO_BYTECODE;
			return ret;
		}

		bytecode->resize(record->blob_size);
		if (!blob.read(record->blob_offset, record->blob_size, bytecode->data())) {
			if (reason)
				*reason = ShaderRegexCacheFailureReason::BLOB_READ_FAILED;
			return ret;
		}
		ret = ShaderRegexCache::PATCH;
	} else
		ret = ShaderRegexCache::MATCH;

	return ret;
}

void ShaderRegexCacheStore::save_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
		bool patched, std::string *asm_text, std::wstring *tagline)
{
	wchar_t path[MAX_PATH];
	FILE *f = NULL;
	size_t suffix;

	if (!G->SHADER_CACHE_PATH[0] || (!G->CACHE_SHADERS && !G->EXPORT_FIXED))
		return;

	if (G->CACHE_SHADERS && !G->DISABLE_REGEX_CACHE) {
		ShaderRegexCacheRecord *record;
		ShaderStage stage;

		stage = shader_stage_from_label(shader_type);
		if (stage == ShaderStage::INVALID)
			return;

		if (!ensure_valid(true)) {
			LogWarning("ShaderRegexCache: save_meta %S %016I64x aborted - cache not usable\n", shader_type, hash);
			return;
		}

		record = find_or_insert_record(stage, hash);
		if (!record) {
			LogWarning("ShaderRegexCache: save_meta %S %016I64x failed to insert record\n", shader_type, hash);
			return;
		}

		// TODO: When we have a condition field in ShaderRegex: The evaluations
		// of *all* valid conditions (not just those matched) must qualify the
		// cache, either by encoding them in the filename or extending the
		// metadata format.

		// The old code deleted the stale .bin file here since the bytecode and
		// metadata were separate per-shader files. In the packed format the
		// record is the single source of truth: clearing patched (and/or the
		// blob_size of a PATCH record that never had its bytecode written) is
		// enough to stop the stale blob block from ever being loaded.
		record->patched = patched;
		record->match_pool_index = (uint32_t)match_pool.size();
		record->match_count = (uint32_t)match_ids->size();
		match_pool.insert(match_pool.end(),
				match_ids->begin(), match_ids->end());

		LogWarning("ShaderRegexCache: save_meta %S %016I64x record updated (matches=%u pool=%u patched=%d)\n",
				shader_type, hash, (unsigned)record->match_count, (unsigned)match_pool.size(), (int)patched);

		// Defer the disk write until the per-frame flush so we don't block
		// the render thread with a full metadata rewrite on every shader:
		metadata_dirty = true;
	}

	if (G->EXPORT_FIXED) {
		suffix = swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_regex.", G->SHADER_CACHE_PATH, hash, shader_type);
		wcscpy_s(path+suffix, MAX_PATH-suffix, L"txt");
		if (patched) {
			wfopen_ensuring_access(&f, path, L"wb");
			if (!f) {
				LogInfo("  Error storing ShaderRegex assembly to %S\n", path);
				return;
			}

			fprintf_s(f, "%S\n", tagline->c_str());
			fwrite(asm_text->c_str(), 1, asm_text->size(), f);

			fclose(f);
			LogInfo("  Storing ShaderRegex assembly to %S\n", path);
		} else {
			if (DeleteFile(path))
				LogInfo("  Removed stale ShaderRegex assembly file %S\n", path);
		}
	}
}

void ShaderRegexCacheStore::save_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode)
{
	ShaderRegexCacheRecord *record;
	ShaderStage stage;

	if (!G->SHADER_CACHE_PATH[0] || !G->CACHE_SHADERS || G->DISABLE_REGEX_CACHE)
		return;
	if (!bytecode || bytecode->empty())
		return;

	stage = shader_stage_from_label(shader_type);
	if (stage == ShaderStage::INVALID)
		return;

	if (!ensure_valid(true)) {
		LogWarning("ShaderRegexCache: save_bin %S %016I64x aborted - cache not usable\n", shader_type, hash);
		return;
	}

	record = find_record(stage, hash);
	if (!record) {
		LogWarning("ShaderRegexCache: save_bin %S %016I64x no record (save_meta should run first)\n", shader_type, hash);
		return; // save_shader_regex_cache_meta() should have run before this
	}

	// The assembly can be re-run (e.g. after a config reload) and usually
	// produces identical bytecode. Skip the append in that common case so the
	// blob does not keep growing on every reload:
	if (record->blob_size == bytecode->size() && record->blob_size) {
		std::vector<byte> existing(bytecode->size());
		if (blob.read(record->blob_offset, record->blob_size, existing.data())
		 && existing == *bytecode)
			return;
	}

	uint64_t blob_offset;
	if (!blob.append(bytecode->data(), bytecode->size(), &blob_offset)) {
		LogWarning("ShaderRegexCache: save_bin %S %016I64x blob append failed (%Iu bytes)\n",
				shader_type, hash, bytecode->size());
		return;
	}

	record->blob_offset = blob_offset;
	record->blob_size = (uint32_t)bytecode->size();
	record->patched = 1;
	header.blob_size = blob.logical_end();

	LogWarning("ShaderRegexCache: save_bin %S %016I64x at blob offset %Iu (%u bytes, logical end %Iu)\n",
			shader_type, hash, (uint64_t)blob_offset, (unsigned)bytecode->size(), (uint64_t)header.blob_size);

	// Defer the disk write until the per-frame flush:
	metadata_dirty = true;
}

// ---- Public API ----

static void save_shader_regex_cache_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
		bool patched, std::string *asm_text, std::wstring *tagline)
{
	std::lock_guard<std::mutex> lock(shader_regex_cache_mutex);
	shader_regex_cache_store.save_meta(hash, shader_type, match_ids, patched, asm_text, tagline);
}

ShaderRegexCache load_shader_regex_cache(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode, std::wstring *tagline, ShaderRegexCacheFailureReason *reason)
{
	std::lock_guard<std::mutex> lock(shader_regex_cache_mutex);
	return shader_regex_cache_store.load(hash, shader_type, bytecode, tagline, reason);
}

void save_shader_regex_cache_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode)
{
	std::lock_guard<std::mutex> lock(shader_regex_cache_mutex);
	shader_regex_cache_store.save_bin(hash, shader_type, bytecode);
}

bool shader_regex_cache_flush()
{
	std::lock_guard<std::mutex> lock(shader_regex_cache_mutex);
	return shader_regex_cache_store.flush();
}

bool get_shader_model_from_bytecode(const void* data, size_t size, std::string* out_model)
{
	if (!data || size < 32 || !out_model)
		return false;

	const uint8_t* buffer = static_cast<const uint8_t*>(data);

	// Validate DXBC header
	if (memcmp(buffer, "DXBC", 4) != 0)
		return false;

	const uint8_t* ptr = buffer + 4 + 16; // Skip FOURCC + hash

	// Read header fields
	if (ptr + 12 > buffer + size)
		return false;

	uint32_t one, totalSize, numChunks;
	memcpy(&one, ptr, 4); ptr += 4;
	memcpy(&totalSize, ptr, 4); ptr += 4;
	memcpy(&numChunks, ptr, 4); ptr += 4;

	if (numChunks == 0)
		return false;

	// Validate chunk table bounds
	if (ptr + numChunks * sizeof(uint32_t) > buffer + size)
		return false;

	const uint32_t* chunkOffsets = reinterpret_cast<const uint32_t*>(ptr);

	// Iterate chunks backwards (same as disassembler)
	for (int32_t i = (int32_t)numChunks - 1; i >= 0; --i)
	{
		uint32_t offset = chunkOffsets[i];

		if (offset + 12 > size)
			continue;

		const uint8_t* chunk = buffer + offset;

		// Look for shader code chunk
		if (memcmp(chunk, "SHEX", 4) != 0 && memcmp(chunk, "SHDR", 4) != 0)
			continue;

		// Version token is at +8
		uint32_t versionToken;
		memcpy(&versionToken, chunk + 8, 4);

		uint32_t type = (versionToken >> 16) & 0xFFFF;
		uint32_t major = (versionToken >> 4) & 0xF;
		uint32_t minor = (versionToken >> 0) & 0xF;

		// Map the DXBC version token's type field to the stage table. Unknown
		// types fall back to the table's "xx" row, matching the old default:
		const char *prefix = shader_stage_info_at(shader_stage_from_dxil_type(type)).model_prefix;

		char buf[16];
		snprintf(buf, sizeof(buf), "%s_%u_%u", prefix, major, minor);

		*out_model = buf;
		return true;
	}

	return false;
}

// Process groups that do not have patches to apply. Those can be handled without disassembly.
void link_shader_regex_groups_without_patterns(const wchar_t* shader_type, std::string* shader_model, UINT64 hash, bool* decompilation_required)
{
	ShaderRegexGroups::iterator i;
	vector<uint32_t> match_ids;
	vector<ShaderRegexGroup*> match_groups;
	uint32_t j;

	for (i = shader_regex_groups.begin(), j = 0; i != shader_regex_groups.end(); i++, j++) {
		ShaderRegexGroup* group = &i->second;

		// Skip group without matching shader model.
		if (!group->shader_models.count(*shader_model)) {
			continue;
		}

		// Skip group with patterns. Those ones need txt to match.
		if (!group->patterns.empty()) {
			if (decompilation_required)
				*decompilation_required = true;
			continue;
		}

		match_ids.push_back(j);
		match_groups.push_back(group);

		LogInfo("ShaderRegex (no pattern): %S %016I64x matches [%S]\n", shader_type, hash, group->ini_section.c_str());
	}

	// If no ShaderRegEx requires decompilation, link CommandLists and update shader cache here instead of `apply_shader_regex_groups`. 
	if (decompilation_required && !*decompilation_required) {
		// Enable CommandList sections execution for this group.
		for (ShaderRegexGroup* group : match_groups) {
			group->link_command_lists_and_filter_index(hash);
		}
		// We save the cache metadata even if we didn't match anything. That
		// way we can skip checking for a match next time when we know there
		// won't be any. This only saves the metadata - the caller will use
		// save_shader_regex_cache_bin to save the assembled binary.
		save_shader_regex_cache_meta(hash, shader_type, &match_ids, false, nullptr, nullptr);
	}
}

bool apply_shader_regex_groups(std::string *asm_text, const wchar_t *shader_type, std::string *shader_model, UINT64 hash, std::wstring *tagline)
{
	ShaderRegexGroups::iterator i;
	ShaderRegexGroup *group;
	bool patched = false;
	bool match, patch;
	vector<uint32_t> match_ids;
	uint32_t j;

	for (i = shader_regex_groups.begin(), j = 0; i != shader_regex_groups.end(); i++, j++) {
		group = &i->second;

		// Skip group without matching shader model.
		if (!group->shader_models.count(*shader_model)) {
			continue;
		}

		// Match/patch only ShaderRegEx with Pattern.
		if (!group->patterns.empty()) {
			// Run patch.
			group->apply_regex_patterns(asm_text, &match, &patch);
			if (!match)
				continue;

			LogInfo("ShaderRegex: %s %016I64x matches [%S]\n", shader_model->c_str(), hash, group->ini_section.c_str());
			patched = patched || patch;

			// Append section to patch sequence.
			if (patch && tagline)
				tagline->append(std::wstring(L"[") + group->ini_section + std::wstring(L"]"));
		}

		match_ids.push_back(j);

		// Enable CommandList sections execution for this group.
		group->link_command_lists_and_filter_index(hash);
	}

	// We save the cache metadata even if we didn't match anything. That
	// way we can skip checking for a match next time when we know there
	// won't be any. This only saves the metadata - the caller will use
	// save_shader_regex_cache_bin to save the assembled binary.
	save_shader_regex_cache_meta(hash, shader_type, &match_ids, patched, asm_text, tagline);

	return patched;
}

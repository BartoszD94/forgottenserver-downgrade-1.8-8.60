#include "../otpch.h"

#include "../game.h"
#include "../item.h"
#include "../outputmessage.h"
#include "../player.h"
#include "../protocolgame.h"
#include "../protocolspectator.h"
#include "../tasks.h"
#include "../tile.h"
#include "test_support.h"

struct ProtocolGameCombatTestAccess
{
	static void configure(ProtocolGame& p, const std::shared_ptr<Player>& viewer, bool astra)
	{
		p.player = viewer;
		p.isAstraClient = astra;
		p.isOTC = astra;
		p.version = 860;
		p.getCurrentBuffer() = std::make_shared<OutputMessage>();
	}
	static void magic(ProtocolGame& p, const Position& pos, uint16_t type) { p.sendMagicEffect(pos, type); }
	static void distance(ProtocolGame& p, const Position& from, const Position& to, uint16_t type)
	{
		p.sendDistanceShoot(from, to, type);
	}
	static void health(ProtocolGame& p, const Creature* c) { p.sendCreatureHealth(c); }
	static void turn(ProtocolGame& p, const Creature* c, uint32_t stack) { p.sendCreatureTurn(c, stack); }
	static void animated(ProtocolGame& p, const Position& pos, std::string_view text)
	{
		p.sendAnimatedText(text, pos, TEXTCOLOR_RED);
	}
	static void text(ProtocolGame& p, const std::string& text) { p.sendTextMessage(MESSAGE_STATUS_DEFAULT, text); }
	static void reset(ProtocolGame& p) { p.getCurrentBuffer()->reset(); }
};

namespace {
const Position CENTER{0x1234, 0x5678, 7};

class PacketCreature final : public Creature
{
public:
	PacketCreature() { id = 0x12345678; }
	const std::string& getName() const override { return name; }
	const std::string& getNameDescription() const override { return name; }
	std::string getDescription(int32_t) const override { return name; }
	CreatureType_t getType() const override { return CREATURETYPE_MONSTER; }
	void setID() override {}
	void removeList() override {}
	void addList() override {}

private:
	std::string name = "packet test creature";
};

Tile* tileAt(const Position& pos, bool ground = true)
{
	if (Item::items.size() == 0) {
		const auto path =
		    std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "data/items/items.otb";
		CHECK(Item::items.loadFromOtb(path.string()));
	}
	if (!g_game.map.getTile(pos)) {
		g_game.map.setTile(pos.x, pos.y, pos.z, std::make_unique<StaticTile>(pos.x, pos.y, pos.z));
	}
	auto* tile = g_game.map.getTile(pos);
	if (ground && !tile->getGround()) {
		auto item = Item::CreateItem(106);
		tile->internalAddThing(item.get());
	}
	return tile;
}

struct Fixture
{
	ProtocolGame protocol{nullptr};
	std::shared_ptr<Player> viewer = std::make_shared<Player>(nullptr);
	std::shared_ptr<PacketCreature> creature = std::make_shared<PacketCreature>();
	explicit Fixture(bool astra = false)
	{
		viewer->setGroup(std::make_shared<Group>());
		viewer->setParent(tileAt(CENTER));
		creature->setParent(tileAt(CENTER));
		ProtocolGameCombatTestAccess::configure(protocol, viewer, astra);
	}
};

std::vector<uint8_t> body(ProtocolGame& p)
{
	const auto& out = p.getCurrentBuffer();
	return {out->getBuffer() + NetworkMessage::INITIAL_BUFFER_POSITION,
	        out->getBuffer() + NetworkMessage::INITIAL_BUFFER_POSITION + out->getLength()};
}

void sameBody(ProtocolGame& p, NetworkMessage& legacy)
{
	CHECK(body(p) == std::vector<uint8_t>(legacy.getBuffer() + 8, legacy.getBuffer() + 8 + legacy.getLength()));
	// The same immutable payload is independently framed in differently dirty storage.
	for (bool checksum : {false, true}) {
		class FrameProtocol final : public Protocol
		{
		public:
			explicit FrameProtocol(bool checksum) : Protocol(nullptr)
			{
				enableXTEAEncryption();
				setXTEAKey({1, 2, 3, 4});
				if (!checksum) {
					disableChecksum();
				}
			}
			void onRecvFirstMessage(NetworkMessage&) override {}
		} frame(checksum);
		auto old = std::make_shared<OutputMessage>(), current = std::make_shared<OutputMessage>();
		std::fill_n(old->getBuffer(), NETWORKMESSAGE_MAXSIZE, 0);
		std::fill_n(current->getBuffer(), NETWORKMESSAGE_MAXSIZE, 0xa5);
		old->append(legacy);
		current->append(body(p));
		frame.onSendMessage(old);
		frame.onSendMessage(current);
		CHECK(old->getLength() == current->getLength());
		CHECK(
		    std::equal(old->getOutputBuffer(), old->getOutputBuffer() + old->getLength(), current->getOutputBuffer()));
	}
}
} // namespace

TEST_CASE(combat_effect_distance_health_and_turn_keep_legacy_bytes)
{
	for (const bool astra : {false, true}) {
		Fixture f(astra);
		ProtocolGameCombatTestAccess::magic(f.protocol, CENTER, 0x1234);
		CHECK(body(f.protocol) == std::vector<uint8_t>({0x83, 0x34, 0x12, 0x78, 0x56, 7, 0x34, 0x12}));
		NetworkMessage legacy;
		legacy.addByte(0x83);
		legacy.addPosition(CENTER);
		legacy.add<uint16_t>(0x1234);
		sameBody(f.protocol, legacy);
		ProtocolGameCombatTestAccess::reset(f.protocol);
		legacy.reset();
		const Position to{0x1235, 0x5679, 7};
		ProtocolGameCombatTestAccess::distance(f.protocol, CENTER, to, 511);
		legacy.addByte(0x85);
		legacy.addPosition(CENTER);
		legacy.addPosition(to);
		legacy.add<uint16_t>(511);
		sameBody(f.protocol, legacy);
		ProtocolGameCombatTestAccess::reset(f.protocol);
		legacy.reset();
		f.creature->setMaxHealth(3);
		f.creature->setHealth(1);
		ProtocolGameCombatTestAccess::health(f.protocol, f.creature.get());
		CHECK(body(f.protocol) == std::vector<uint8_t>({0x8c, 0x78, 0x56, 0x34, 0x12, 34}));
		legacy.addByte(0x8c);
		legacy.add<uint32_t>(f.creature->getID());
		legacy.addByte(34);
		sameBody(f.protocol, legacy);
		ProtocolGameCombatTestAccess::reset(f.protocol);
		legacy.reset();
		f.creature->setDirection(DIRECTION_EAST);
		ProtocolGameCombatTestAccess::turn(f.protocol, f.creature.get(), 2);
		legacy.addByte(0x6b);
		legacy.addPosition(CENTER);
		legacy.addByte(2);
		legacy.add<uint16_t>(0x63);
		legacy.add<uint32_t>(f.creature->getID());
		legacy.addByte(DIRECTION_EAST);
		sameBody(f.protocol, legacy);
	}
}

TEST_CASE(combat_texts_keep_encoding_and_per_field_fallback)
{
	Fixture f;
	for (const std::string& text :
	     {std::string("damage -123"), std::string("caf\xc3\xa9"), std::string("\xf0\x9f\x98\x80"), std::string("\xc3"),
	      std::string(8192, 'x'), std::string(8193, 'x'), std::string()}) {
		ProtocolGameCombatTestAccess::reset(f.protocol);
		ProtocolGameCombatTestAccess::animated(f.protocol, CENTER, text);
		NetworkMessage legacy;
		legacy.addByte(0x84);
		legacy.addPosition(CENTER);
		legacy.addByte(TEXTCOLOR_RED);
		legacy.addString(text);
		sameBody(f.protocol, legacy);
		ProtocolGameCombatTestAccess::reset(f.protocol);
		legacy.reset();
		ProtocolGameCombatTestAccess::text(f.protocol, text);
		legacy.addByte(0xb4);
		legacy.addByte(MESSAGE_STATUS_DEFAULT);
		legacy.addString(text);
		sameBody(f.protocol, legacy);
	}
}

TEST_CASE(combat_packet_visibility_and_hidden_health_are_unchanged)
{
	Fixture f;
	for (const Position& invisible : {Position{1, 1, 7}, Position{CENTER.x, CENTER.y, 8}}) {
		ProtocolGameCombatTestAccess::magic(f.protocol, invisible, 1);
		ProtocolGameCombatTestAccess::animated(f.protocol, invisible, "1");
		CHECK(body(f.protocol).empty());
	}
	const Position withoutGround{static_cast<uint16_t>(CENTER.x + 1), CENTER.y, 7};
	tileAt(withoutGround, false);
	ProtocolGameCombatTestAccess::magic(f.protocol, withoutGround, 1);
	CHECK(body(f.protocol).empty());
	ProtocolGameCombatTestAccess::turn(f.protocol, f.creature.get(), MAX_STACKPOS_THINGS);
	CHECK(body(f.protocol).empty());
	f.creature->setInstanceID(123);
	ProtocolGameCombatTestAccess::turn(f.protocol, f.creature.get(), 0);
	CHECK(body(f.protocol).empty());
	f.creature->setHiddenHealth(true);
	ProtocolGameCombatTestAccess::health(f.protocol, f.creature.get());
	CHECK(body(f.protocol) == std::vector<uint8_t>({0x8c, 0x78, 0x56, 0x34, 0x12, 0}));
}

int main()
{
	g_dispatcher.start();
	int result = EXIT_FAILURE;
	g_dispatcher.addTask([&result] {
		result = tfs::tests::run();
		g_dispatcher.shutdown();
	});
	g_reactor.runLoop();
	return result;
}

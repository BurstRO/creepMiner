﻿// ==========================================================================
// 
// creepMiner - Burstcoin cryptocurrency CPU and GPU miner
// Copyright (C)  2016-2018 Creepsky (creepsky@gmail.com)
// 
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110 - 1301  USA
// 
// ==========================================================================

#include "MinerData.hpp"
#include "MinerConfig.hpp"
#include "logging/MinerLogger.hpp"
#include "shabal/MinerShabal.hpp"
#include "logging/Output.hpp"
#include "MinerUtil.hpp"
#include "wallet/Wallet.hpp"
#include "wallet/Account.hpp"

using namespace Poco::Data::Keywords;

Burst::BlockData::BlockData(const Poco::UInt64 blockHeight, const Poco::UInt64 baseTarget, const std::string& genSigStr,
                            MinerData* parent, const Poco::UInt64 blockTargetDeadline)
	: blockHeight_ {blockHeight},
	  baseTarget_ {baseTarget},
	  blockTargetDeadline_{ blockTargetDeadline },
	  genSigStr_ {genSigStr},
	  parent_{parent}
{
	poco_ndc(BlockData::BlockData);

	try
	{
		entries_ = std::make_shared<std::vector<Poco::JSON::Object>>();
		//deadlines_ = std::make_shared<std::unordered_map<AccountId, Deadlines>>();

		for (auto i = 0; i < 32; ++i)
		{
			const auto byteStr = genSigStr.substr(i * 2, 2);
			genSig_[i] = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16));
		}

		Shabal256_SSE2 hash;
		GensigData newGenSig;
		
		hash.update(&genSig_[0], genSig_.size());
		hash.update(blockHeight);
		hash.close(&newGenSig[0]);

		roundTime_ = 0;
		scoop_ = (static_cast<int>(newGenSig[newGenSig.size() - 2] & 0x0F) << 8) | static_cast<int>(newGenSig[newGenSig.size() - 1]);
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not create the block data: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		throw;
	}
}

bool Burst::BlockData::addDeadlineUnlocked(const std::shared_ptr<Deadline>& deadline)
{
	poco_ndc(BlockData::BlockData);

	try
	{
		auto accountId = deadline->getAccountId();
		
		auto iter = deadlines_.find(accountId);

		if (iter == deadlines_.end())
		{
			deadlines_.insert(std::make_pair(accountId, std::make_shared<Deadlines>(this)));
			deadlines_[accountId]->add(deadline);
		}
		else
		{
			iter->second->add(deadline);
		}

		return true;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not add the new deadline: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return false;
	}
}

bool Burst::BlockData::addDeadline(const std::shared_ptr<Deadline>& deadline)
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
	return addDeadlineUnlocked(deadline);
}

void Burst::BlockData::setBaseTarget(Poco::UInt64 baseTarget)
{
	baseTarget_ = baseTarget;
}

void Burst::BlockData::confirmedDeadlineEvent(const std::shared_ptr<Deadline>& deadline)
{
	if (deadline == nullptr)
		return;

	addBlockEntry(createJsonDeadline(*deadline, "nonce confirmed"));

	// set the best deadline for this block
	{
		Poco::ScopedLock<Poco::Mutex> lock{mutex_};

		if (bestDeadline_ == nullptr ||
			bestDeadline_->getDeadline() > deadline->getDeadline())
			bestDeadline_ = deadline;
	}
}

Burst::BlockData::DataLoader::DataLoader()
	: getLastWinner{this, &DataLoader::runGetLastWinner}
{}

Burst::BlockData::DataLoader::~DataLoader() = default;

Burst::BlockData::DataLoader& Burst::BlockData::DataLoader::getInstance()
{
	static DataLoader data_loader;
	return data_loader;
}

std::shared_ptr<Burst::Account> Burst::BlockData::DataLoader::runGetLastWinner(const std::tuple<const Wallet&, Accounts&, BlockData&>& args)
{
	poco_ndc(BlockData::DataLoader::runGetLastWinner);

	try
	{
		auto& wallet = std::get<0>(args);
		auto& accounts = std::get<1>(args);
		auto& blockdata = std::get<2>(args);
		
		AccountId lastWinner;
		const auto lastBlockheight = blockdata.blockHeight_ - 1;
		
		if (!wallet.isActive())
			return nullptr;

		if (wallet.getWinnerOfBlock(lastBlockheight, lastWinner))
		{
			auto winnerAccount = accounts.getAccount(lastWinner, wallet, false);

			winnerAccount->getOrLoadName().wait();
			winnerAccount->getOrLoadRewardRecipient().wait();

			std::string rewardRecipient;

			if (winnerAccount->getRewardRecipient() == winnerAccount->getId())
				rewardRecipient = "                   Solo mining";
			else
			{
				auto rewardRecipientAccount = accounts.getAccount(winnerAccount->getRewardRecipient(), wallet, false);
				rewardRecipientAccount->getOrLoadName().wait();
				rewardRecipient = "Pool               " + rewardRecipientAccount->getName();
			}

			log_ok_if(MinerLogger::miner, MinerLogger::hasOutput(LastWinner), std::string(50, '-') + "\n"
				"last block winner: \n"
				"block#             %s\n"
				"winner-numeric     %Lu\n"
				"winner-address     %s\n"
				"%s" +
				"%s\n" +
				std::string(50, '-'),
				numberToString(lastBlockheight), lastWinner, winnerAccount->getAddress(),
				winnerAccount->getName().empty() ? "" : Poco::format("winner-name        %s\n", winnerAccount->getName()),
				rewardRecipient
			);

			blockdata.setLastWinner(winnerAccount);

			return winnerAccount;
		}

		return nullptr;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not load the last winner: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return nullptr;
	}
}

void Burst::BlockData::setLastWinner(const std::shared_ptr<Account>& account)
{
	poco_ndc(BlockData::setLastWinner);

	try
	{
		// set the winner for the last block
		{
			Poco::ScopedLock<Poco::Mutex> lock{mutex_};
			lastWinner_ = account;
		}

		auto lastWinnerJson = account->toJSON();
		lastWinnerJson->set("type", "lastWinner");
		addBlockEntry(*lastWinnerJson);
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not set the last winner: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
	}
}

void Burst::BlockData::refreshBlockEntry() const
{
	if (parent_ != nullptr)
		addBlockEntry(createJsonNewBlock(*parent_));
}

void Burst::BlockData::refreshConfig() const
{
	addBlockEntry(createJsonConfig());
}

void Burst::BlockData::refreshPlotDirs() const
{
	addBlockEntry(createJsonPlotDirsRescan());
}

void Burst::BlockData::setProgress(const float progressRead, const float progressVerification, const Poco::UInt64 blockheight)
{
	poco_ndc(BlockData::setProgress);

	try
	{
		if (blockheight != getBlockheight())
			return;

		{
			Poco::ScopedLock<Poco::Mutex> lock{mutex_};
			jsonProgress_ = new Poco::JSON::Object{createJsonProgress(progressRead, progressVerification)};
		}

		if (parent_ != nullptr)
			parent_->blockDataChangedEvent.notify(this, *jsonProgress_);
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not set the progress: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
	}
}

void Burst::BlockData::setProgress(const std::string& plotDir, const float progress, const Poco::UInt64 blockheight)
{
	poco_ndc(BlockData::setProgress);

	try
	{
		if (blockheight != getBlockheight())
			return;

		Poco::ScopedLock<Poco::Mutex> lock{mutex_};
		auto json = new Poco::JSON::Object{ createJsonProgress(progress, 0.f) };
		json->set("type", "plotdir-progress");
		json->set("dir", plotDir);

		jsonDirProgress_[plotDir].assign(json);

		if (parent_ != nullptr)
			parent_->blockDataChangedEvent.notify(this, *json);
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not set the progress: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
	}
}

void Burst::BlockData::setRoundTime(double rTime)
{
	roundTime_ = rTime;
}

void Burst::BlockData::addBlockEntry(Poco::JSON::Object entry) const
{
	poco_ndc(BlockData::addBlockEntry);

	try
	{
		{
			Poco::ScopedLock<Poco::Mutex> lock{mutex_};
			entries_->emplace_back(entry);	
		}
		
		if (parent_ != nullptr)
			parent_->blockDataChangedEvent.notify(this, entry);	
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not add the block entry: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
	}
}

Poco::UInt64 Burst::BlockData::getBlockheight() const
{
	return blockHeight_.load();
}

Poco::UInt64 Burst::BlockData::getScoop() const
{
	return scoop_.load();
}

Poco::UInt64 Burst::BlockData::getBasetarget() const
{
	return baseTarget_.load();
}

Poco::UInt64 Burst::BlockData::getDifficulty() const
{
	return 18325193796 / getBasetarget();
}

double Burst::BlockData::getRoundTime() const
{
	return roundTime_;
}

Poco::UInt64 Burst::BlockData::getBlockTargetDeadline() const
{
	return blockTargetDeadline_.load();
}

std::shared_ptr<Burst::Account> Burst::BlockData::getLastWinner() const
{
	return lastWinner_;
}
 
const Burst::GensigData& Burst::BlockData::getGensig() const
{
	return genSig_;
}

const std::string& Burst::BlockData::getGensigStr() const
{
	return genSigStr_;
}

std::shared_ptr<Burst::Deadline> Burst::BlockData::getBestDeadline() const
{
	return bestDeadline_;
}

std::shared_ptr<Burst::Deadline> Burst::BlockData::getBestDeadline(const DeadlineSearchType searchType) const
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
	std::shared_ptr<Deadline> bestDeadline;

	poco_ndc(BlockData::getBestDeadline);

	try
	{
		for (const auto& accountDeadlines : deadlines_)
		{
			if (accountDeadlines.second == nullptr)
				continue;
		
			std::shared_ptr<Deadline> accountBestDeadline;

			if (searchType == DeadlineSearchType::Found)
				accountBestDeadline = accountDeadlines.second->getBestFound();
			else if (searchType == DeadlineSearchType::Sent)
				accountBestDeadline = accountDeadlines.second->getBestSent();
			else if (searchType == DeadlineSearchType::Confirmed)
				accountBestDeadline = accountDeadlines.second->getBestConfirmed();

			if (accountBestDeadline == nullptr)
				continue;

			if (bestDeadline == nullptr ||
				bestDeadline->getDeadline() > accountBestDeadline->getDeadline())
				bestDeadline = accountBestDeadline;
		}

		return bestDeadline;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get the best specific deadline: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return nullptr;
	}
}

bool Burst::BlockData::forEntries(const std::function<bool(const Poco::JSON::Object&)>& traverseFunction) const
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
	poco_ndc(BlockData::forEntries);

	try
	{
		if (entries_ == nullptr)
			return false;

		auto error = false;

		for (auto iter = entries_->begin(); iter != entries_->end() && !error; ++iter)
			error = !traverseFunction(*iter);

		// send the overall progress, if any
		if (!error && !jsonProgress_.isNull())
			error = !traverseFunction(*jsonProgress_);

		// send the dir progress
		if (!error)
			for (auto iter = jsonDirProgress_.begin(); !error && iter != jsonDirProgress_.end(); ++iter)
				error = !traverseFunction(*iter->second);

		return error;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not traverse the entries: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return false;
	}
}

//std::vector<Poco::JSON::Object> Burst::BlockData::getEntries() const
//{
//	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
//	return *entries_;
//}

//const std::unordered_map<Burst::AccountId, Burst::Deadlines>& Burst::BlockData::getDeadlines() const
//{
//	Poco::ScopedLock<Poco::FastMutex> lock{mutex_};
//	return deadlines_;
//}

std::shared_ptr<Burst::Deadline> Burst::BlockData::getBestDeadlineUnlocked(const Poco::UInt64 accountId, const DeadlineSearchType searchType)
{
	poco_ndc(BlockData::getBestDeadlineUnlocked);

	try
	{
		const auto iter = deadlines_.find(accountId);

		if (iter == deadlines_.end())
			return nullptr;

		switch (searchType)
		{
		case DeadlineSearchType::Found:
			return iter->second->getBestFound();
		case DeadlineSearchType::Sent:
			return iter->second->getBestSent();
		case DeadlineSearchType::Confirmed:
			return iter->second->getBestConfirmed();
		default:
			return nullptr;
		}
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get the best deadline: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return nullptr;
	}
}

std::shared_ptr<Burst::Deadline> Burst::BlockData::getBestDeadline(const Poco::UInt64 accountId, const DeadlineSearchType searchType)
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
	return getBestDeadlineUnlocked(accountId, searchType);
}

Poco::ActiveResult<std::shared_ptr<Burst::Account>> Burst::BlockData::getLastWinnerAsync(const Wallet& wallet, Accounts& accounts)
{
	return DataLoader::getInstance().getLastWinner(make_tuple(std::cref(wallet), std::ref(accounts), std::ref(*this)));
}

bool Burst::BlockData::addDeadlineIfBest(const std::shared_ptr<Deadline>& deadline)
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};

	poco_ndc(BlockData::addDeadlineIfBest);

	try
	{
		const auto bestDeadline = getBestDeadlineUnlocked(deadline->getAccountId(), DeadlineSearchType::Found);

		if (bestDeadline == nullptr || bestDeadline->getDeadline() > deadline->getDeadline())
			return addDeadlineUnlocked(deadline);

		return false;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not add a new (best) deadline: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return false;
	}
}

void Burst::BlockData::addMessage(const Poco::Message& message) const
{
	Poco::JSON::Object json;
	poco_ndc(BlockData::addMessage);

	try
	{
		{
			Poco::ScopedLock<Poco::Mutex> lock{mutex_};

			if (entries_ == nullptr)
				return;
			
			json.set("type", std::to_string(static_cast<int>(message.getPriority())));
			json.set("text", message.getText());
			json.set("source", message.getSource());
			json.set("line", message.getSourceLine());
			json.set("file", message.getSourceFile());
			json.set("time", Poco::DateTimeFormatter::format(Poco::LocalDateTime(message.getTime()), "%H:%M:%S"));

			entries_->emplace_back(json);
		}

		if (parent_ != nullptr)
			parent_->blockDataChangedEvent.notify(this, json);	
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not add a new message: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
	}
}

void Burst::BlockData::clearEntries() const
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
	entries_->clear();
}

bool Burst::BlockData::forDeadlines(const std::function<bool(const Deadline&)>& traverseFunction) const
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};

	poco_ndc(BlockData::forDeadlines);

	try
	{
		if (deadlines_.empty())
			return false;

		auto error = false;

		for (auto iterAccounts = deadlines_.begin(); iterAccounts != deadlines_.end() && !error; ++iterAccounts)
		{
			if (iterAccounts->second == nullptr)
				continue;

			const auto deadlines = iterAccounts->second->getDeadlines();

			for (auto iter = deadlines.begin(); iter != deadlines.end() && !error; ++iter)
				error = traverseFunction(**iter);
		}

		return error;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not traverse deadlines: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return false;
	}
}

Burst::MinerData::MinerData()
	: blocksWon_(0),
	  activityWonBlocks_{this, &MinerData::runGetWonBlocks}
{
	poco_ndc(MinerData::MinerData);

	const auto databasePath = MinerConfig::getConfig().getDatabasePath();

	try
	{
		dbSession_ = std::make_unique<Poco::Data::Session>("SQLite", databasePath);
		
		*dbSession_ <<
			"CREATE TABLE IF NOT EXISTS deadline (" <<
			"	id				INTEGER NOT NULL," <<
			"	height			INTEGER NOT NULL," <<
			"	account			INTEGER NOT NULL," <<
			"	nonce			INTEGER NOT NULL," <<
			"	value			INTEGER NOT NULL," <<
			"	file			TEXT NOT NULL," <<
			"	miner			TEXT NOT NULL," <<
			"	totalplotsize	REAL NOT NULL," <<
			"	status			INTEGER NOT NULL," <<
			"	PRIMARY KEY (id)" <<
			")", now;

		*dbSession_ <<
			"CREATE TABLE IF NOT EXISTS block (" <<
			"	id				INTEGER NOT NULL," <<
			"	height			INTEGER NOT NULL," <<
			"	scoop			INTEGER NOT NULL," <<
			"	baseTarget		INTEGER NOT NULL," <<
			"	gensig			TEXT NOT NULL," <<
			"	difficulty		INTEGER NOT NULL," <<
			"	targetDeadline	INTEGER NOT NULL," <<
			"	roundTime		REAL NOT NULL," <<
			"	blockTime		REAL NOT NULL," <<
			"	PRIMARY KEY (id)" <<
			")", now;
	}
	catch (Poco::Exception& e)
	{
		throw Poco::Exception{Poco::format("Could not load/create the database '%s'\n\tReason: %s", databasePath, e.displayText())};
	}
}

Burst::MinerData::~MinerData() = default;

std::shared_ptr<Burst::BlockData> Burst::MinerData::startNewBlock(Poco::UInt64 block, Poco::UInt64 baseTarget,
                                                                  const std::string& genSig,
                                                                  Poco::UInt64 blockTargetDeadline)
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};

	// save the old data in the historical container
	if (blockData_ != nullptr)
	{
		// add the block to the database
		try
		{
			*dbSession_ <<
				"INSERT INTO block VALUES (NULL, :height, :scoop, :btarget, :gensig, :diff, :targdl, :roundt, :blockt)",
				bind(blockData_->getBlockheight()), bind(blockData_->getScoop()), bind(blockData_->getBasetarget()),
				useRef(blockData_->getGensigStr()), bind(blockData_->getDifficulty()), bind(blockData_->getBlockTargetDeadline()),
				bind(blockData_->getRoundTime()), bind(blockData_->getBlockTime()), now;

			blockData_->forDeadlines([this](const Deadline& deadline)
			{
				try
				{
					const auto status = [&]()
					{
						if (deadline.isConfirmed())
							return 3;

						if (deadline.isSent())
							return 2;

						if (deadline.isOnTheWay())
							return 1;

						return 0;
					}();

					*dbSession_ <<
						"INSERT INTO deadline VALUES (NULL, :height, :account, :nonce, :value, :file, :miner, :totalplotsize, :status)",
						bind(deadline.getBlock()), bind(deadline.getAccountId()), bind(deadline.getNonce()), bind(deadline.getDeadline()),
						bind(deadline.getPlotFile()), bind(deadline.getMiner()), bind(deadline.getTotalPlotsize()), bind(status), now;
				}
				catch (Poco::Exception& e)
				{
					log_error(MinerLogger::general, "Could not insert deadline %s for block %Lu\n\tReason: %s",
						deadline.deadlineToReadableString(), deadline.getBlock(), e.displayText());
				}

				return false;
			});
		}
		catch (Poco::Exception& e)
		{
			log_error(MinerLogger::general, "Could not insert block %Lu\n\tReason: %s",
				blockData_->getBlockheight(), e.displayText());
		}
	}

	try
	{
		lastBlockData_ = blockData_;
		blockData_ = std::make_shared<BlockData>(block, baseTarget, genSig, this, blockTargetDeadline);
		return blockData_;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not create new block data: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return nullptr;
	}
}

std::vector<std::shared_ptr<Burst::BlockData>> Burst::MinerData::getHistoricalBlocks(const Poco::UInt64 from, const Poco::UInt64 to) const
{
	std::vector<std::shared_ptr<BlockData>> historicalBlocks;
	forAllBlocks(from, to, [&](const auto& block)
	{
		historicalBlocks.emplace_back(block);
		return false;
	});
	return historicalBlocks;
}

void Burst::MinerData::forAllBlocks(const Poco::UInt64 from, const Poco::UInt64 to,
	const std::function<bool(std::shared_ptr<BlockData>&)>& traverseFunction) const
{
	poco_ndc(MinerData::forAllBlocks);

	try
	{
		std::vector<Poco::UInt64> nonces, values, accounts, totalPlotSizes, status;
		std::vector<std::string> files;

		std::vector<Poco::UInt64> heights, baseTargets, targetDeadlines, blockTimes;
		std::vector<double> roundTimes;
		std::vector<std::string> gensigs;

		const auto fetchAll = from == 0 && to == 0;
		std::string query = "SELECT height, baseTarget, gensig, targetDeadline, roundTime, blockTime FROM block";

		if (!fetchAll)
			query += " WHERE height >= :from AND height <= :to";

		auto stmt = (*dbSession_ << query,	into(heights), into(baseTargets), into(gensigs),
											into(targetDeadlines), into(roundTimes), into(blockTimes));

		if (!fetchAll)
		{
			stmt.bind(from);
			stmt.bind(to);
		}

		stmt.execute();

		Poco::UInt64 height;

		auto stmtDeadlines = (*dbSession_ << "SELECT nonce, value, account, file, totalplotsize, status " <<
											 "FROM deadline WHERE height = :height",
			into(nonces), into(values), into(accounts), into(files), into(totalPlotSizes), into(status), use(height));

		auto stop = false;

		for (size_t i = 0; i < heights.size() && !stop; ++i)
		{
			nonces.clear();
			values.clear();
			accounts.clear();
			files.clear();
			totalPlotSizes.clear();
			status.clear();

			height = heights[i];

			auto historicBlock = std::make_shared<BlockData>(
				height,
				baseTargets[i],
				gensigs[i],
				nullptr,
				targetDeadlines[i]
			);

			historicBlock->setRoundTime(roundTimes[i]);
			historicBlock->setBlockTime(blockTimes[i]);

			stmtDeadlines.execute();

			for (size_t j = 0; j < nonces.size(); ++j)
			{
				auto deadline = std::make_shared<Deadline>(nonces[j], values[j], std::make_shared<Account>(accounts[j]), height, files[j]);
				historicBlock->addDeadline(deadline);

				switch (status[j])
				{
				case 3:
					deadline->confirm();
				case 2:
					deadline->send();
				case 1:
					deadline->onTheWay();
				default:
					break;
				}
			}

			stop = traverseFunction(historicBlock);
		}
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not use function for all blocks: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
	}
}

Poco::UInt64 Burst::MinerData::runGetWonBlocks(const std::pair<const Wallet*, const Accounts*>& args)
{
	poco_ndc(BlockData::runGetWonBlocks);

	try
	{
		auto& wallet = *args.first;
		auto& accounts = *args.second;
		size_t wonBlocks = 0;

		if (!wallet.isActive())
			return 0;

		std::vector<Block> blocks;

		for (auto& account : accounts.getAccounts())
			if (wallet.getAccountBlocks(account->getId(), blocks))
				wonBlocks += blocks.size();

		bool refresh;

		{
			Poco::ScopedLock<Poco::Mutex> lock{mutex_};
			const auto before = blocksWon_.load();
			blocksWon_.store(wonBlocks);
			refresh = before != wonBlocks;
		}

		if (blockData_ != nullptr && refresh)
			blockData_->refreshBlockEntry();

		return wonBlocks;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get the number of won block: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return 0;
	}
}

void Burst::MinerData::addMessage(const Poco::Message& message)
{
	const auto blockData = getBlockData();

	if (blockData != nullptr)
		blockData->addMessage(message);
}

std::shared_ptr<Burst::Deadline> Burst::MinerData::getBestDeadlineOverall(bool onlyHistorical) const
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};

	try
	{
		Poco::UInt64 from = 0, to = 0;

		if (onlyHistorical)
		{
			if (blockData_ == nullptr)
				*dbSession_ << "SELECT MAX(height) FROM block", into(to), now;
			else
				to = blockData_->getBlockheight();

			if (to == 0)
				return nullptr;

			const auto maxHistoricalBlocks = MinerConfig::getConfig().getMaxHistoricalBlocks();

			if (maxHistoricalBlocks > to)
				from = 0;
			else
				from = to - MinerConfig::getConfig().getMaxHistoricalBlocks();
		}

		std::string query = "SELECT nonce, value, account, height, file, MIN(value) FROM deadline WHERE status = 3 ";
		
		if (onlyHistorical)
			query += " AND height >= :from AND height < :to";

		Poco::UInt64 nonce, value, account, height, minValue;
		std::string file;

		auto stmt = (*dbSession_ << query, into(nonce), into(value), into(account), into(height), into(file), into(minValue));

		if (onlyHistorical)
		{
			stmt.bind(from);
			stmt.bind(to);
		}

		stmt.execute();

		if (file.empty())
			return nullptr;

		return std::make_shared<Deadline>(nonce, value, std::make_shared<Account>(account), height, file);
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get best overall deadline: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return nullptr;
	}
}

const Poco::Timestamp& Burst::MinerData::getStartTime() const
{
	return startTime_;
}

Poco::Timespan Burst::MinerData::getRunTime() const
{
	return Poco::Timestamp{} - getStartTime();
}

void Burst::BlockData::setBlockTime(Poco::UInt64 bTime)
{
	blockTime_ = bTime;
}

Poco::UInt64 Burst::BlockData::getBlockTime() const
{
	return blockTime_;
}

Poco::UInt64 Burst::MinerData::getBlocksMined() const
{
	poco_ndc(MinerData::getBlocksMined);

	try
	{
		Poco::UInt64 blocksMined;
		*dbSession_ << "SELECT COUNT(*) FROM block", into(blocksMined), now;
		return blocksMined;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get mined blocks: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return 0;
	}
}

Poco::UInt64 Burst::MinerData::getBlocksWon() const
{
	return blocksWon_.load();
}

std::shared_ptr<Burst::BlockData> Burst::MinerData::getBlockData()
{
	return blockData_;
}

std::shared_ptr<const Burst::BlockData> Burst::MinerData::getBlockData() const
{
	return blockData_;
}

std::shared_ptr<const Burst::BlockData> Burst::MinerData::getHistoricalBlockData(const Poco::UInt32 roundsBefore) const
{
	poco_ndc(MinerData::getHistoricalBlockData);

	try
	{
		if (blockData_ == nullptr)
			return nullptr;

		if (roundsBefore == 0)
			return blockData_;

		const auto currentHeight = getCurrentBlockheight();
		const auto from = [&]()
		{
			const auto maxHistoricalBlocks = MinerConfig::getConfig().getMaxHistoricalBlocks();
			
			if (maxHistoricalBlocks > currentHeight)
				return Poco::UInt64{0};

			return currentHeight - maxHistoricalBlocks;
		}();

		std::shared_ptr<const BlockData> historicBlock;

		forAllBlocks(from, currentHeight, [&](const auto& block)
		{
			historicBlock = block;
			return true;
		});

		return historicBlock;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get historical block data: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return nullptr;
	}
}

//std::shared_ptr<const Poco::JSON::Object> Burst::MinerData::getLastWinner() const
//{
//	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
//
//	// we make a local copy here, because the block data could be changed
//	// every second and we would have undefined behaviour
//	auto blockData = getBlockData();
//
//	if (blockData == nullptr)
//		return nullptr;
//
//	return blockData->getLastWinner();
//}

std::vector<std::shared_ptr<Burst::BlockData>> Burst::MinerData::getAllHistoricalBlockData() const
{
	poco_ndc(MinerData::getAllHistoricalBlockData);

	try
	{
		const auto from = getCurrentBlockheight() - MinerConfig::getConfig().getMaxHistoricalBlocks();
		const auto to = getCurrentBlockheight();
		return getHistoricalBlocks(from, to);
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get all historical block data: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return {};
	}
}

Poco::UInt64 Burst::MinerData::getConfirmedDeadlines() const
{
	poco_ndc(MinerData::getConfirmedDeadlines);

	try
	{
		Poco::UInt64 deadlinesConfirmed;
		*dbSession_ << "SELECT COUNT(*) FROM deadline WHERE status = 3", into(deadlinesConfirmed), now;
		return deadlinesConfirmed;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get confirmed deadlines: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return 0;
	}
}

Poco::UInt64 Burst::MinerData::getAverageDeadline() const
{
	poco_ndc(MinerData::getAverageDeadline);

	try
	{
		Poco::UInt64 avg = 0;
		*dbSession_ << "SELECT AVG(value) FROM deadline WHERE status = 3", into(avg), now;
		return avg;
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get average deadline: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return 0;
	}
}

Poco::Int64 Burst::MinerData::getDifficultyDifference() const
{
	poco_ndc(MinerData::getDifficultyDifference);

	try
	{
		if (blockData_ == nullptr)
			return 0;

		const Poco::Int64 difficulty = blockData_->getDifficulty();

		if (lastBlockData_ == nullptr)
			return difficulty;
		
		return difficulty - lastBlockData_->getDifficulty();
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get difficulty difference: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return 0;
	}
}

Burst::HighscoreValue<Poco::UInt64> Burst::MinerData::getLowestDifficulty() const
{
	poco_ndc(MinerData::getLowestDifficulty);

	try
	{
		Poco::UInt64 height, difficulty;
		*dbSession_ << "SELECT height, MIN(difficulty) FROM block", into(height), into(difficulty), now;
		return {height, difficulty};
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get lowest difficulty: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return {};
	}
}

Burst::HighscoreValue<Poco::UInt64> Burst::MinerData::getHighestDifficulty() const
{
	poco_ndc(MinerData::getHighestDifficulty);

	try
	{
		Poco::UInt64 height, difficulty;
		*dbSession_ << "SELECT height, MAX(difficulty) FROM block", into(height), into(difficulty), now;
		return {height, difficulty};
	}
	catch (const Poco::Exception& e)
	{
		log_error(MinerLogger::miner, "Could not get highest difficulty: %s", e.displayText());
		log_current_stackframe(MinerLogger::miner);
		return {};
	}
}

Poco::UInt64 Burst::MinerData::getCurrentBlockheight() const
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
	return blockData_ == nullptr ? 0 : blockData_->getBlockheight();
}

Poco::UInt64 Burst::MinerData::getCurrentBasetarget() const
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
	return blockData_ == nullptr ? 0 : blockData_->getBasetarget();
}

Poco::UInt64 Burst::MinerData::getCurrentScoopNum() const
{
	Poco::ScopedLock<Poco::Mutex> lock{mutex_};
	return blockData_ == nullptr ? 0 : blockData_->getScoop();
}

Poco::ActiveResult<Poco::UInt64> Burst::MinerData::getWonBlocksAsync(const Wallet& wallet, const Accounts& accounts)
{
	const auto tuple = std::make_pair(&wallet, &accounts);
	return activityWonBlocks_(tuple);
}

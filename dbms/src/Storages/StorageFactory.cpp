#include <Poco/Util/Application.h>
#include <Poco/Util/AbstractConfiguration.h>

#include <DB/Parsers/ASTCreateQuery.h>
#include <DB/Parsers/ASTIdentifier.h>
#include <DB/Parsers/ASTLiteral.h>

#include <DB/Interpreters/Context.h>

#include <DB/Storages/StorageLog.h>
#include <DB/Storages/StorageTinyLog.h>
#include <DB/Storages/StorageMemory.h>
#include <DB/Storages/StorageMerge.h>
#include <DB/Storages/StorageMergeTree.h>
#include <DB/Storages/StorageDistributed.h>
#include <DB/Storages/StorageSystemNumbers.h>
#include <DB/Storages/StorageSystemOne.h>
#include <DB/Storages/StorageFactory.h>
#include <DB/Storages/StorageChunks.h>
#include <DB/Storages/StorageChunkRef.h>
#include <DB/Storages/StorageChunkMerger.h>

#include <DB/DataTypes/DataTypeArray.h>
#include <DB/DataTypes/DataTypeNested.h>


namespace DB
{

StoragePtr StorageFactory::get(
	const String & name,
	const String & data_path,
	const String & table_name,
	const String & database_name,
	Context & context,
	ASTPtr & query,
	NamesAndTypesListPtr columns,
	bool attach) const
{
	columns = DataTypeNested::expandNestedColumns(*columns);
	
	if (name == "Log")
	{
		return StorageLog::create(data_path, table_name, columns);
	}
	else if (name == "Chunks")
	{
		return StorageChunks::create(data_path, table_name, database_name, columns, context, attach);
	}
	else if (name == "ChunkRef")
	{
		throw Exception("Table with storage ChunkRef must not be created manually.", ErrorCodes::TABLE_MUST_NOT_BE_CREATED_MANUALLY);
	}
	else if (name == "ChunkMerger")
	{
		ASTs & args_func = dynamic_cast<ASTFunction &>(*dynamic_cast<ASTCreateQuery &>(*query).storage).children;
		
		do
		{
			if (args_func.size() != 1)
				break;
			
			ASTs & args = dynamic_cast<ASTExpressionList &>(*args_func.at(0)).children;
			
			if (args.size() < 3 || args.size() > 4)
				break;
			
			String source_database = dynamic_cast<ASTIdentifier &>(*args[0]).name;
			String source_table_name_regexp = safeGet<const String &>(dynamic_cast<ASTLiteral &>(*args[1]).value);
			size_t chunks_to_merge = safeGet<UInt64>(dynamic_cast<ASTLiteral &>(*args[2]).value);
			
			String destination_name_prefix = "group_";
			String destination_database = source_database;
			
			if (args.size() > 3)
				destination_name_prefix = dynamic_cast<ASTIdentifier &>(*args[3]).name;
			
			return StorageChunkMerger::create(database_name, table_name, columns, source_database, source_table_name_regexp, destination_name_prefix, chunks_to_merge, context);
		} while(false);
		
		throw Exception("Storage ChunkMerger requires from 3 to 4 parameters:"
			" source database, regexp for source table names, number of chunks to merge, [destination tables name prefix].",
			ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
	}
	else if (name == "TinyLog")
	{
		return StorageTinyLog::create(data_path, table_name, columns, attach);
	}
	else if (name == "Memory")
	{
		return StorageMemory::create(table_name, columns);
	}
	else if (name == "Merge")
	{
		/** В запросе в качестве аргумента для движка указано имя БД, в которой находятся таблицы-источники,
		  *  а также регексп для имён таблиц-источников.
		  */
		ASTs & args_func = dynamic_cast<ASTFunction &>(*dynamic_cast<ASTCreateQuery &>(*query).storage).children;

		if (args_func.size() != 1)
			throw Exception("Storage Merge requires exactly 2 parameters"
				" - name of source database and regexp for table names.",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

		ASTs & args = dynamic_cast<ASTExpressionList &>(*args_func.at(0)).children;

		if (args.size() != 2)
			throw Exception("Storage Merge requires exactly 2 parameters"
				" - name of source database and regexp for table names.",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

		String source_database 		= dynamic_cast<ASTIdentifier &>(*args[0]).name;
		String table_name_regexp	= safeGet<const String &>(dynamic_cast<ASTLiteral &>(*args[1]).value);
		
		return StorageMerge::create(table_name, columns, source_database, table_name_regexp, context);
	}
	else if (name == "Distributed")
	{
		/** В запросе в качестве аргумента для движка указано имя конфигурационной секции,
		  *  в которой задан список удалённых серверов, а также имя удалённой БД и имя удалённой таблицы.
		  */
		ASTs & args_func = dynamic_cast<ASTFunction &>(*dynamic_cast<ASTCreateQuery &>(*query).storage).children;

		if (args_func.size() != 1)
			throw Exception("Storage Distributed requires 3 or 4 parameters"
				" - name of configuration section with list of remote servers, name of remote database, name of remote table[, sign column name].",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
		
		ASTs & args = dynamic_cast<ASTExpressionList &>(*args_func.at(0)).children;
		
		if (args.size() != 3 && args.size() != 4)
			throw Exception("Storage Distributed requires 3 or 4 parameters"
				" - name of configuration section with list of remote servers, name of remote database, name of remote table[, sign column name].",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);
		
		String config_name 		= dynamic_cast<ASTIdentifier &>(*args[0]).name;
		String remote_database 	= dynamic_cast<ASTIdentifier &>(*args[1]).name;
		String remote_table 	= dynamic_cast<ASTIdentifier &>(*args[2]).name;
		String sign_column_name	= args.size() == 4 ? dynamic_cast<ASTIdentifier &>(*args[3]).name : "";

		/** В конфиге адреса либо находятся в узлах <node>:
		  * <node>
		  * 	<host>example01-01-1</host>
		  * 	<port>9000</port>
		  * 	<!-- <user>, <password>, если нужны -->
		  * </node>
		  * ...
		  * либо в узлах <shard>, и внутри - <replica>
		  * <shard>
		  * 	<replica>
		  * 		<host>example01-01-1</host>
		  * 		<port>9000</port>
		  * 		<!-- <user>, <password>, если нужны -->
		  *		</replica>
		  * </shard>
		  */
		StorageDistributed::Addresses addresses;
		StorageDistributed::AddressesWithFailover addresses_with_failover;

		Poco::Util::AbstractConfiguration & config = Poco::Util::Application::instance().config();
		Poco::Util::AbstractConfiguration::Keys config_keys;
		config.keys("remote_servers." + config_name, config_keys);

		String config_prefix = "remote_servers." + config_name + ".";
			
		for (Poco::Util::AbstractConfiguration::Keys::const_iterator it = config_keys.begin(); it != config_keys.end(); ++it)
		{
			if (0 == strncmp(it->c_str(), "node", strlen("node")))
			{
				addresses.push_back(StorageDistributed::Address(
					Poco::Net::SocketAddress(
						config.getString(config_prefix + *it + ".host"),
						config.getInt(config_prefix + *it + ".port")),
					config.getString(config_prefix + *it + ".user", "default"),
					config.getString(config_prefix + *it + ".password", "")));
			}
			else if (0 == strncmp(it->c_str(), "shard", strlen("shard")))
			{
				Poco::Util::AbstractConfiguration::Keys replica_keys;
				config.keys(config_prefix + *it, replica_keys);

				addresses_with_failover.push_back(StorageDistributed::Addresses());
				StorageDistributed::Addresses & replica_addresses = addresses_with_failover.back();
				
				for (Poco::Util::AbstractConfiguration::Keys::const_iterator jt = replica_keys.begin(); jt != replica_keys.end(); ++jt)
				{
					if (0 == strncmp(jt->c_str(), "replica", strlen("replica")))
						replica_addresses.push_back(StorageDistributed::Address(
							Poco::Net::SocketAddress(
								config.getString(config_prefix + *it + "." + *jt + ".host"),
								config.getInt(config_prefix + *it + "." + *jt + ".port")),
							config.getString(config_prefix + *it + ".user", "default"),
							config.getString(config_prefix + *it + ".password", "")));
					else
						throw Exception("Unknown element in config: " + *jt, ErrorCodes::UNKNOWN_ELEMENT_IN_CONFIG);
				}
			}
			else
				throw Exception("Unknown element in config: " + *it, ErrorCodes::UNKNOWN_ELEMENT_IN_CONFIG);
		}

		if (!addresses_with_failover.empty() && !addresses.empty())
			throw Exception("There must be either 'node' or 'shard' elements in config", ErrorCodes::EXCESSIVE_ELEMENT_IN_CONFIG);
		
		if (!addresses_with_failover.empty())
			return StorageDistributed::create(table_name, columns, addresses_with_failover, remote_database, remote_table,
										  context.getDataTypeFactory(), context.getSettings(), context, sign_column_name);
		else if (!addresses.empty())
			return StorageDistributed::create(table_name, columns, addresses, remote_database, remote_table,
										  context.getDataTypeFactory(), context.getSettings(), context, sign_column_name);
		else
			throw Exception("No addresses listed in config", ErrorCodes::NO_ELEMENTS_IN_CONFIG);
	}
	else if (name == "MergeTree" || name == "SummingMergeTree")
	{
		/** В качестве аргумента для движка должно быть указано:
		  *  - имя столбца с датой;
		  *  - имя столбца для семплирования (запрос с SAMPLE x будет выбирать строки, у которых в этом столбце значение меньше, чем x*UINT32_MAX);
		  *  - выражение для сортировки в скобках;
		  *  - index_granularity.
		  * Например: ENGINE = MergeTree(EventDate, intHash32(UniqID), (CounterID, EventDate, intHash32(UniqID), EventTime), 8192).
		  * 
		  * SummingMergeTree - вариант, в котором при слиянии делается суммирование всех числовых столбцов кроме PK
		  *  - для Баннерной Крутилки.
		  */
		ASTs & args_func = dynamic_cast<ASTFunction &>(*dynamic_cast<ASTCreateQuery &>(*query).storage).children;

		if (args_func.size() != 1)
			throw Exception("Storage " + name + " requires 3 or 4 parameters"
				" - name of column with date, [name of column for sampling], primary key expression, index granularity.",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

		ASTs & args = dynamic_cast<ASTExpressionList &>(*args_func.at(0)).children;

		if (args.size() != 3 && args.size() != 4)
			throw Exception("Storage " + name + " requires 3 or 4 parameters"
				" - name of column with date, [name of column for sampling], primary key expression, index granularity.",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

		size_t arg_offset = args.size() - 3;

		String date_column_name 	= dynamic_cast<ASTIdentifier &>(*args[0]).name;
		ASTPtr sampling_expression = arg_offset == 0 ? NULL : args[1];
		UInt64 index_granularity	= safeGet<UInt64>(dynamic_cast<ASTLiteral &>(*args[arg_offset + 2]).value);
		ASTFunction & primary_expr_func = dynamic_cast<ASTFunction &>(*args[arg_offset + 1]);
		
		if (primary_expr_func.name != "tuple")
			throw Exception("Primary expression for storage " + name + " must be in parentheses.",
				ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

		ASTPtr primary_expr = primary_expr_func.children.at(0);

		return StorageMergeTree::create(
			data_path, table_name, columns, context, primary_expr, date_column_name, sampling_expression, index_granularity,
			name == "SummingMergeTree" ? StorageMergeTree::Summing : StorageMergeTree::Ordinary);
	}
	else if (name == "CollapsingMergeTree")
	{
		/** В качестве аргумента для движка должно быть указано:
		  *  - имя столбца с датой;
		  *  - имя столбца для семплирования (запрос с SAMPLE x будет выбирать строки, у которых в этом столбце значение меньше, чем x*UINT32_MAX);
		  *  - выражение для сортировки в скобках;
		  *  - index_granularity;
		  *  - имя столбца, содержащего тип строчки с изменением "визита" (принимающего значения 1 и -1).
		  * Например: ENGINE = CollapsingMergeTree(EventDate, (CounterID, EventDate, intHash32(UniqID), VisitID), 8192, Sign).
		  */
		ASTs & args_func = dynamic_cast<ASTFunction &>(*dynamic_cast<ASTCreateQuery &>(*query).storage).children;

		if (args_func.size() != 1)
			throw Exception("Storage CollapsingMergeTree requires 4 or 5 parameters"
				" - name of column with date, [name of column for sampling], primary key expression, index granularity, sign_column.",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

		ASTs & args = dynamic_cast<ASTExpressionList &>(*args_func.at(0)).children;

		if (args.size() != 4 && args.size() != 5)
			throw Exception("Storage CollapsingMergeTree requires 4 or 5 parameters"
				" - name of column with date, [name of column for sampling], primary key expression, index granularity, sign_column.",
				ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH);

		size_t arg_offset = args.size() - 4;
		
		String date_column_name 	= dynamic_cast<ASTIdentifier &>(*args[0]).name;
		ASTPtr sampling_expression = arg_offset == 0 ? NULL : args[1];
		UInt64 index_granularity	= safeGet<UInt64>(dynamic_cast<ASTLiteral &>(*args[arg_offset + 2]).value);
		String sign_column_name 	= dynamic_cast<ASTIdentifier &>(*args[arg_offset + 3]).name;
		ASTFunction & primary_expr_func = dynamic_cast<ASTFunction &>(*args[arg_offset + 1]);

		if (primary_expr_func.name != "tuple")
			throw Exception("Primary expression for storage CollapsingMergeTree must be in parentheses.",
				ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

		ASTPtr primary_expr = primary_expr_func.children.at(0);

		return StorageMergeTree::create(
			data_path, table_name, columns, context, primary_expr, date_column_name,
			sampling_expression, index_granularity, StorageMergeTree::Collapsing, sign_column_name);
	}
	else if (name == "SystemNumbers")
	{
		if (columns->size() != 1 || columns->begin()->first != "number" || columns->begin()->second->getName() != "UInt64")
			throw Exception("Storage SystemNumbers only allows one column with name 'number' and type 'UInt64'",
				ErrorCodes::ILLEGAL_COLUMN);

		return StorageSystemNumbers::create(table_name);
	}
	else if (name == "SystemOne")
	{
		if (columns->size() != 1 || columns->begin()->first != "dummy" || columns->begin()->second->getName() != "UInt8")
			throw Exception("Storage SystemOne only allows one column with name 'dummy' and type 'UInt8'",
				ErrorCodes::ILLEGAL_COLUMN);

		return StorageSystemOne::create(table_name);
	}
	else
		throw Exception("Unknown storage " + name, ErrorCodes::UNKNOWN_STORAGE);
}


}

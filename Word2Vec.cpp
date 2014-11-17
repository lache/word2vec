﻿#include "Word2Vec.h"

inline bool comp(Word *w1, Word *w2)
{
	return w1->count > w2->count;
}

Word2Vec::~Word2Vec(void)
{
}

Word2Vec::Word2Vec(int iter, int window, int min_count, int table_size, int word_dim, int negative,
		float subsample_threshold, float init_alpha, float min_alpha, bool cbow_mean, string train_method, string model):
		iter(iter),  window(window), min_count(min_count), table_size(table_size), word_dim(word_dim),
		negative(negative), subsample_threshold(subsample_threshold), init_alpha(init_alpha),
		min_alpha(min_alpha), train_method(train_method), model(model), generator(rd()), distribution_window(0, window - 1),
		distribution_table(0, table_size - 1){} 

vector<vector<string>> Word2Vec::line_docs(string filename)
{
	vector<vector<string>> sentences;
	ifstream in(filename);
	string s;
	while (std::getline(in, s))
	{
		istringstream iss(s);
		sentences.emplace_back(istream_iterator<string>{iss}, istream_iterator<string>{});
	}
	return std::move(sentences);
}

void Word2Vec::create_huffman_tree()
{
	size_t vocab_size = vocab.size();

	vector<Word *> heap = vocab;
	make_heap(heap.begin(), heap.end(), comp);

	for(size_t i = 0; i < vocab_size - 1; ++i)
	{
		pop_heap(heap.begin(), heap.end(), comp);
		Word *min_left = heap.back(); heap.pop_back();
		pop_heap(heap.begin(), heap.end(), comp);
		Word *min_right = heap.back(); heap.pop_back();

		Word *w = new Word(i + vocab_size, min_left->count + min_right->count, "", min_left, min_right);
		heap.push_back(w);
		push_heap(heap.begin(), heap.end(), comp);
	}

	//traverse huffman tree，get code
	list<tuple<Word *, vector<size_t>, vector<size_t>>> stack;
	stack.push_back(make_tuple(heap[0],  vector<size_t>(), vector<size_t>()));
	while(!stack.empty())
	{
		auto n = stack.back();
		stack.pop_back();

		Word *n_w = get<0>(n);
		if(n_w->index < vocab_size)
		{
			n_w->codes = get<1>(n);
			n_w->points = get<2>(n);
		}
		else
		{
			auto codes_left = get<1>(n);
			auto codes_right = codes_left;
			codes_left.push_back(0); 
			codes_right.push_back(1);

			auto points = get<2>(n);
			points.emplace_back(n_w->index - vocab_size);

			stack.emplace_back(make_tuple(n_w->left, codes_left, points));
			stack.emplace_back(make_tuple(n_w->right, codes_right, points));
		}
	}
}


void Word2Vec::make_table()
{
	table.resize(table_size);
	size_t vocab_size = vocab.size();
	float power = 0.75f;
	double train_words_pow = 0.0;

	for(const auto& v: vocab)
		train_words_pow += pow((float)v->count, power);

	size_t idx = 0;
	double d1 = pow(float(vocab[idx]->count), power) / train_words_pow;
	double scope = table_size * d1;
	for(int i = 0; i < table_size; ++i)
	{
		table[i] = idx;
		if (i > scope && idx < vocab_size - 1)
		{
			d1 += pow((float)vocab[++idx]->count, power) / train_words_pow;
			scope = table_size * d1;
		}
		else if (idx == vocab_size - 1)
		{
			for(; i < table_size; ++i)
				table[i] = idx;
			break;
		}
	}
}

void Word2Vec::precalc_sampling()
{
	size_t vocab_size = vocab.size();
	long total_words = 0;
	for(auto v: vocab)
		total_words +=v->count;

	float threshold_count  = subsample_threshold * total_words;

	if(subsample_threshold > 0)
		for(const auto& v: vocab)
			v->sample_probability = std::min(float((sqrt(v->count / threshold_count) + 1) * threshold_count / v->count), (float)1.0);
	else
		for(const auto& v: vocab)
			v->sample_probability = 1.0;
}

void Word2Vec::build_vocab(vector<vector<string>> &sentences)
{
	unordered_map<string, int> word_cn;

	for(auto& sentence: sentences)
		for(auto& w: sentence)
			if(word_cn.count(w) > 0)
				word_cn[w]++;
			else
				word_cn[w] = 1;

	for(auto kv: word_cn)
	{
		if(kv.second < min_count)
			continue;

		Word *w = new Word(0, kv.second,  kv.first);
		vocab.push_back(w);
		vocab_hash[w->text] = WordP(w);
	}

	//update word index
	size_t vocab_size = vocab.size();
	sort(vocab.begin(), vocab.end(), comp);
	for(uint32_t i = 0; i < vocab_size; i++)
	{
		vocab[i]->index = i;
		idx2word.push_back(vocab[i]->text);
	}

	if(train_method == "hs")
		create_huffman_tree();

	if(negative)
		make_table();

	precalc_sampling();
}

void Word2Vec::save_vocab(string vocab_filename)
{
	ofstream out(vocab_filename, std::ofstream::out);
	for(auto& v: vocab)
		out << v->index << " " << v->count << " " << v->text << endl;
	out.close();
}

void Word2Vec::read_vocab(string vocab_filename)
{
	ifstream in(vocab_filename);
	string s;

	while (std::getline(in, s))
	{
		istringstream iss(s);
		size_t index, count;
		string text;

		iss >> index >> count >>text;
		Word *w = new Word(index, count, text);
		vocab.push_back(w);
		vocab_hash[w->text] = WordP(w);
	}
	in.close();
}

void Word2Vec::init_weights(size_t vocab_size)
{
	std::uniform_real_distribution<float> distribution(-0.5, 0.5);
	auto uniform = [&] (int) {return distribution(generator);};

	W = RMatrixXf::NullaryExpr(vocab_size, word_dim, uniform);
	W = W / (float)word_dim ;

	if(train_method == "hs")
		synapses1 =RMatrixXf::Zero(vocab_size - 1, word_dim);
	else if(train_method == "ns")
		synapses1_neg = RMatrixXf::Zero(vocab_size, word_dim);
}

vector<vector<Word *>> Word2Vec::build_sample(vector<vector<string>> & data)
{
	vector<vector<Word *>> samples;
	std::uniform_real_distribution<float> distribution(0.0, 1.0);

	for(auto& sentence: data)
	{
		vector<Word *> sampled;

		for(auto text: sentence)
		{
			auto it = vocab_hash.find(text);
			if (it == vocab_hash.end()) continue;
			Word *word = it->second.get();

			if(subsample_threshold > 0 && word->sample_probability < distribution(generator))
				continue;

			sampled.push_back(word);
		}
		samples.push_back(std::move(sampled));
	}
	return std::move(samples);
}

RowVectorXf& Word2Vec::hierarchical_softmax(Word * predict_word, RowVectorXf& project_rep, RowVectorXf& project_grad, float alpha)
{
	size_t code_len = predict_word->codes.size();

	for(int i = 0; i < code_len; ++i)
	{
		size_t current_idx = predict_word->points[i];
		float f = synapses1.row(current_idx).dot(project_rep);
		f = 1.0 / (1 + exp(-f));
		float g = (1 - predict_word->codes[i] - f) * alpha;
		// Propagate errors output -> hidden
		project_grad += g * synapses1.row(current_idx);
		// Learn weights hidden -> output
		synapses1.row(current_idx) += g * project_rep;
	}
	return project_grad;
}

RowVectorXf& Word2Vec::negative_sampling(Word * predict_word, RowVectorXf& project_rep, RowVectorXf& project_grad, float alpha)
{
	unordered_map<size_t, uint8_t> targets;
	for (int j = 0; j < negative; ++j)
		targets[table[distribution_table(generator)]] = 0;

	targets[predict_word->index] = 1;

	for (auto it: targets)
	{
		auto l2 = synapses1_neg.row(it.first);
		float f = l2.dot(project_rep);
		f = 1.0 / (1 + exp(-f));
		float g = (it.second - f) * alpha;
		// Propagate errors output -> hidden
		project_grad += g * l2;
		// Learn weights hidden -> output
		l2 += g * project_rep;
	}
	return project_grad;
}

void Word2Vec::train_sentence_cbow(vector<Word *>& sentence, float alpha)
{
	RowVectorXf neu1 = RowVectorXf::Zero(word_dim);
	RowVectorXf neu1_grad = RowVectorXf::Zero(word_dim);
	size_t len = sentence.size();

	for (int i = 0; i < len; ++i)
	{
		int reduced_window = distribution_window(generator);
		int index_begin = max(0, i - window + reduced_window);
		int index_end = min((int)len, i + window + 1 - reduced_window);
		int neu1_num = index_end - index_begin - 1;
		if (neu1_num) continue;
		//input->projecten
		neu1.setZero();
		neu1_grad.setZero();
		set<size_t> idx;
		for(int j = index_begin; j < index_end; ++j)
			if(j != i)
				idx.insert(sentence[j]->index);
			
		for(auto id: idx) neu1 += W.row(id);
		if(cbow_mean)
			neu1 /= (float)neu1_num;

		if(train_method == "hs")
		{
			neu1_grad = hierarchical_softmax(sentence[i], neu1, neu1_grad, alpha);
		}
		if (negative > 0)
		{
			neu1_grad = negative_sampling(sentence[i], neu1, neu1_grad, alpha);
		}
		// hidden -> in
		if(cbow_mean)
			neu1_grad /= (float)neu1_num;
		for(auto id: idx)  W.row(id) += neu1_grad;
	}
}

void Word2Vec::train_sentence_sg(vector<Word *>& sentence, float alpha)
{
	RowVectorXf neu1 = RowVectorXf::Zero(word_dim);
	RowVectorXf neu1_grad = RowVectorXf::Zero(word_dim);

	auto len = sentence.size();
	for (int i = 0; i < len; ++i)
	{
		neu1_grad.setZero();
		neu1 = W.row(sentence[i]->index);
		int reduced_window = distribution_window(generator);
		int index_begin = max(0, i - window + reduced_window);
		int index_end = min((int)len, i + window + 1 - reduced_window);

		for(int j = index_begin; j < index_end; ++j)
		{
			if(j == i) continue;
			if(train_method == "hs")
			{
				neu1_grad = hierarchical_softmax(sentence[j], neu1, neu1_grad, alpha);
			}
			if(negative > 0)
			{
				neu1_grad = negative_sampling(sentence[j], neu1, neu1_grad, alpha);
			}
		}
		W.row(sentence[i]->index) += neu1_grad;
	}
}


void Word2Vec::train(vector<vector<string>> &sentences)
{
	init_weights(vocab.size());
	long long current_words = 0;
	long long train_words = 0;

	for(auto& sentence: sentences)
		train_words += sentence.size();

	for(int it = 0; it < iter; ++it)
	{
		vector<vector<Word *>> samples = build_sample(sentences);

        #pragma omp parallel for
		for(int i = 0; i < samples.size(); ++i)
		{
			float alpha = std::max(min_alpha, float(init_alpha * (1.0 - 1.0 / iter * current_words / train_words)));
			if(i % 100 == 0)
			{
				printf("init_alpha: %f  Progress: %f%% \n", alpha, 100.0 / iter * current_words / train_words);
				std::fflush(stdout);
			}
			if(model == "cbow")
				train_sentence_cbow(samples[i], alpha);
			else if(model == "sg")
				train_sentence_sg(samples[i], alpha);

			#pragma omp atomic
			current_words += sentences[i].size();
		}
	}
}

void Word2Vec::save_word2vec(string filename, const RMatrixXf& data, bool binary)
{
	IOFormat CommaInitFmt(StreamPrecision, DontAlignCols);

	if(binary)
	{
		std::ofstream out(filename, std::ios::binary);
		char blank = ' ';
		char enter = '\n'; 
		int size = sizeof(char);
		int r_size = data.cols() * sizeof(RMatrixXf::Scalar);

		RMatrixXf::Index r = data.rows();
		RMatrixXf::Index c = data.cols();
		out.write((char*) &r, sizeof(RMatrixXf::Index));
		out.write(&blank, size);
		out.write((char*) &c, sizeof(RMatrixXf::Index));
		out.write(&enter, size);

		for(auto v: vocab)
		{
			out.write(v->text.c_str(), v->text.size());
			out.write(&blank, size);
			out.write((char*) data.row(v->index).data(), r_size);
			out.write(&enter, size);
		}
		out.close();
	}
	else
	{
		ofstream out(filename);

		out << data.rows() << " " << data.cols() << std::endl;

		for(auto v: vocab)
		{
			out << v->text << " " << data.row(v->index).format(CommaInitFmt) << endl;;
		}
		out.close();
	}
}

void Word2Vec::load_word2vec(string filename, bool binary)
{
	if(binary)
	{
		ifstream in(filename, std::ios::binary);

		char temp_char;
		int size = sizeof(char);
		
		RMatrixXf::Index r;
		RMatrixXf::Index c;

		in.read((char*) &r, sizeof(RMatrixXf::Index));
		in.read(&temp_char, size);
		in.read((char*) &c, sizeof(RMatrixXf::Index));
		in.read(&temp_char, size);
		
		int r_size = c * sizeof(RMatrixXf::Scalar);

		for(int i = 0; i < r; ++i)
		{
			string text = "";
			in.read(&temp_char, size);
			while(temp_char !=  ' ')
			{
				text += temp_char;
				in.read(&temp_char, size);
			}
			in.read((char*) W.row(vocab_hash[text]->index).data(), r_size);
			in.read(&temp_char, size);
		}
		in.close();
	}
	else
	{
		ifstream in(filename);
		string s, text;
		std::getline(in, s);
		size_t vocab_size, word_dim;
		istringstream iss(s);
		iss >> vocab_size >> word_dim;

		while (std::getline(in, s))
		{
			istringstream iss(s);
			iss >> text;
			auto w2v = W.row(vocab_hash[text]->index);

			for(int i = 0; i < word_dim; ++i)
			{
				iss >> w2v[i];
			}
		}
		in.close();
	}
}
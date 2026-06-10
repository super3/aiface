module.exports = [
  {
    type: 'heading',
    defaultValue: 'AiFace Settings'
  },
  {
    type: 'section',
    items: [
      {
        type: 'input',
        messageKey: 'APIKEY',
        label: 'OpenRouter API Key',
        defaultValue: '',
        attributes: {
          placeholder: 'sk-or-v1-...'
        }
      },
      {
        type: 'input',
        messageKey: 'MODEL',
        label: 'Model',
        defaultValue: 'anthropic/claude-haiku-4.5',
        description: 'Any model id from openrouter.ai/models'
      }
    ]
  },
  {
    type: 'submit',
    defaultValue: 'Save'
  }
];
